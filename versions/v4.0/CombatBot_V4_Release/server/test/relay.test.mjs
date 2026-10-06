/**
 * @file    relay.test.mjs
 * @brief   使用真实 HTTPS/WSS 连接验证中继权限、租约、重放与失联行为。
 *
 * 范围：真实 Node 服务与真实 ws 客户端；设备仅替身，不替代 ESP32 板端 TLS 验收。
 * 证书：测试期间生成临时自签证书，客户端明确信任该测试 CA，禁止关闭 TLS 校验。
 */
import test from 'node:test';
import assert from 'node:assert/strict';
import https from 'node:https';
import net from 'node:net';
import { mkdtemp, readFile, rm, access } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { randomBytes } from 'node:crypto';
import { WebSocket } from 'ws';
import { createRelay, normalizeBasePath } from '../src/relay.mjs';
import { keyDigest, validateConfig } from '../src/auth.mjs';

const token = () => randomBytes(16).toString('hex');
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
async function freePort() {
  const server = net.createServer();
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const port = server.address().port;
  await new Promise(resolve => server.close(resolve));
  return port;
}
async function opensslPath() {
  for (const candidate of [process.env.OPENSSL_BIN, 'C:/Program Files/Git/usr/bin/openssl.exe',
    'C:/Program Files/Git/mingw64/bin/openssl.exe', 'openssl'].filter(Boolean)) {
    if (spawnSync(candidate, ['version'], { stdio: 'ignore' }).status === 0) return candidate;
  }
  throw new Error('TLS 集成测试需要 OpenSSL，可通过 OPENSSL_BIN 指定路径');
}
function request(url, { ca, origin, cookie, method = 'GET', body } = {}) {
  return new Promise((resolve, reject) => {
    const data = body === undefined ? undefined : JSON.stringify(body);
    const req = https.request(url, { ca, method, headers: {
      Connection: 'close', ...(origin ? { Origin: origin } : {}), ...(cookie ? { Cookie: cookie } : {}),
      ...(data ? { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(data) } : {}),
    } }, res => {
      let content = '';
      res.on('data', chunk => content += chunk);
      res.on('end', () => resolve({ status: res.statusCode, headers: res.headers,
        body: content.startsWith('{') ? JSON.parse(content) : content }));
    });
    req.on('error', reject); if (data) req.write(data); req.end();
  });
}
function connect(url, options) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url, options);
    const messages = [], pending = [];
    ws.on('error', reject);
    ws.on('unexpected-response', (_req, res) => {
      res.resume(); ws.terminate(); reject(new Error(`upgrade ${res.statusCode}`));
    });
    ws.on('message', data => {
      const message = JSON.parse(data.toString());
      const index = pending.findIndex(item => item.predicate(message));
      if (index >= 0) { const item = pending.splice(index, 1)[0]; clearTimeout(item.timer); item.resolve(message); }
      else messages.push(message);
    });
    ws.once('open', () => resolve({ ws, messages,
      send: value => ws.send(JSON.stringify(value)),
      next(predicate, timeout = 2000) {
        const index = messages.findIndex(predicate);
        if (index >= 0) return Promise.resolve(messages.splice(index, 1)[0]);
        return new Promise((done, fail) => {
          const item = { predicate, resolve: done, timer: null };
          item.timer = setTimeout(() => { const i = pending.indexOf(item); if (i >= 0) pending.splice(i, 1);
            fail(new Error('等待消息超时；现有类型=' + messages.map(v => v.t).join(','))); }, timeout);
          pending.push(item);
        });
      },
      closed() { return ws.readyState === WebSocket.CLOSED ? Promise.resolve()
        : new Promise(done => ws.once('close', done)); },
    }));
  });
}

test('路径与配置必须明确，拒绝不安全公网入口', () => {
  assert.equal(normalizeBasePath('/combatbot/'), '/combatbot');
  assert.equal(normalizeBasePath('/'), '');
  assert.throws(() => normalizeBasePath('/../private'));
  assert.throws(() => normalizeBasePath('/combatbot?x=1'));
  assert.throws(() => createRelay({ publicOrigin: 'http://example.com', config: {} }));
  const valid = { deviceId: 'car1', keySha256: keyDigest('a'.repeat(64)), controlKeySha256: keyDigest('b'.repeat(64)) };
  assert.equal(validateConfig(valid).deviceId, 'car1');
  assert.throws(() => validateConfig({ devices: [], users: [] }));
  assert.throws(() => validateConfig({ ...valid, deviceId: 123 }));
  assert.throws(() => validateConfig({ ...valid, controlKeySha256: valid.keySha256 }));
});

test('真实 TLS / HTTP / WebSocket 中继集成', { timeout: 20000 }, async t => {
  const dir = await mkdtemp(path.join(tmpdir(), 'combatbot-relay-test-'));
  const openssl = await opensslPath();
  const certFile = path.join(dir, 'test.crt'), keyFile = path.join(dir, 'test.key');
  const generated = spawnSync(openssl, ['req', '-x509', '-newkey', 'rsa:2048', '-nodes',
    '-keyout', keyFile, '-out', certFile, '-days', '1', '-subj', '/CN=localhost',
    '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1'], { encoding: 'utf8' });
  assert.equal(generated.status, 0, generated.stderr);
  const cert = await readFile(certFile), key = await readFile(keyFile);
  const port = await freePort(), origin = `https://127.0.0.1:${port}`;
  const prefix = origin, wsPrefix = prefix.replace('https:', 'wss:');
  const deviceKey = randomBytes(32).toString('hex'), controlKey = randomBytes(32).toString('hex');
  const relay = createRelay({ basePath: '', publicOrigin: origin,
    tls: { cert, key }, leaseTimeoutMs: 300, webFile: new URL('../../combatbot/web/index.html', import.meta.url),
    config: { deviceId: 'car1', keySha256: keyDigest(deviceKey), controlKeySha256: keyDigest(controlKey) } });
  await new Promise(resolve => relay.server.listen(port, '127.0.0.1', resolve));
  const clients = [], timers = [];
  t.after(async () => {
    timers.forEach(clearInterval); clients.forEach(client => client.ws.terminate());
    await relay.close();
    assert(dir.startsWith(path.resolve(tmpdir()) + path.sep + 'combatbot-relay-test-'));
    await rm(dir, { recursive: true, force: true });
  });
  const req = (suffix, options) => request(prefix + suffix, { ca: cert, ...options });
  async function connectControl() {
    const res = await req('/api/connect', { method: 'POST', origin, body: { controlKey } });
    assert.equal(res.status, 200); assert.equal(res.body.device.deviceId, 'car1');
    return res.headers['set-cookie'][0].split(';')[0];
  }
  const driverCookie = await connectControl(), observerCookie = await connectControl(), rivalCookie = await connectControl();
  async function operator(cookie) {
    const client = await connect(wsPrefix + '/ws/operator', { ca: cert, origin, headers: { Cookie: cookie } });
    clients.push(client); client.welcome = await client.next(v => v.t === 'welcome'); return client;
  }
  async function device(deviceId = 'car1') {
    const client = await connect(wsPrefix + '/ws/device', { ca: cert,
      headers: { 'X-CombatBot-Device': deviceId, 'X-CombatBot-Key': deviceKey } });
    clients.push(client);
    client.boot = token(); client.link = token(); client.permit = token();
    client.send({ t: 'device_hello', boot: client.boot, link: client.link, permit: client.permit, permitTtlMs: 600 });
    await client.next(v => v.t === 'ready');
    const timer = setInterval(() => {
      if (client.ws.readyState !== WebSocket.OPEN) return;
      client.permit = token(); client.send({ t: 'permit', boot: client.boot, link: client.link,
        permit: client.permit, permitTtlMs: 600 });
    }, 100);
    timers.push(timer); client.timer = timer;
    return client;
  }
  function command(client, lease, dev, seq, payload, extra = {}) {
    client.send({ t: 'command', deviceId: 'car1', lease: lease.lease, seq,
      permit: dev.permit, command: payload, ...extra });
  }
  const car = await device();
  let currentCar = car;
  const driver = await operator(driverCookie), observer = await operator(observerCookie), rival = await operator(rivalCookie);

  await t.test('HTTPS证书正常校验，未信任测试CA的客户端失败', async () => {
    assert.equal((await req('/api/info')).body.wsOperator, '/ws/operator');
    await assert.rejects(request(prefix + '/api/info'), /certificate|self.signed/i);
    assert.equal((await request(origin + '/combatbot/api/info', { ca: cert })).status, 404);
  });
  await t.test('控制链接换Secure Cookie，缺失/错误密钥、Origin及设备凭据拒绝', async () => {
    const me = await req('/api/me', { cookie: driverCookie });
    assert.equal(me.status, 200); assert.equal(me.body.device.deviceId, 'car1');
    assert.equal('user' in me.body, false); assert.equal('devices' in me.body, false);
    for (const secret of [deviceKey, controlKey]) assert.equal(JSON.stringify(me.body).includes(secret), false);
    const info = (await req('/api/info')).body;
    assert.equal(info.singleDevice, true); assert.equal(info.deviceId, 'car1'); assert.equal(info.basePath, '');
    assert.equal('loginRequired' in info, false);
    assert.equal((await req('/api/me')).status, 401);
    assert.equal((await req('/api/login', { method: 'POST', origin, body: {} })).status, 404);
    assert.equal((await req('/api/connect', { method: 'POST', origin: 'https://evil.example', body: { controlKey } })).status, 403);
    assert.equal((await req('/api/connect', { method: 'POST', origin, body: {} })).status, 401);
    assert.equal((await req('/api/connect', { method: 'POST', origin, body: { controlKey: 'not-a-key' } })).status, 401);
    assert.equal((await req('/api/connect', { method: 'POST', origin, body: { controlKey: deviceKey } })).status, 401);
    const connected = await req('/api/connect', { method: 'POST', origin, body: { controlKey } });
    assert.match(connected.headers['set-cookie'][0], /HttpOnly; Secure; SameSite=Strict/);
    await assert.rejects(connect(wsPrefix + '/ws/operator', { ca: cert, origin }), /upgrade 401/);
    await assert.rejects(connect(wsPrefix + '/ws/operator', { ca: cert, origin: 'https://evil.example', headers: { Cookie: driverCookie } }), /upgrade 401/);
    await assert.rejects(connect(wsPrefix + '/ws/device', { ca: cert, headers: { 'X-CombatBot-Device': 'car2', 'X-CombatBot-Key': deviceKey } }), /upgrade 401/);
    await assert.rejects(connect(wsPrefix + '/ws/device', { ca: cert, headers: { 'X-CombatBot-Device': 'car1', 'X-CombatBot-Key': controlKey } }), /upgrade 401/);
    await assert.rejects(connect(wsPrefix + '/ws/device', { ca: cert, headers: { 'X-CombatBot-Device': 'car1', 'X-CombatBot-Key': deviceKey } }), /upgrade 409/);
  });
  await t.test('唯一车辆自动订阅；未持租约不能停止或急停，错误编号无原型越权', async () => {
    assert.equal(observer.welcome.device.deviceId, 'car1');
    assert.equal('devices' in observer.welcome, false); assert.equal('user' in observer.welcome, false);
    observer.send({ t: 'command', deviceId: 'car1', command: { t: 'estop' } });
    assert.equal((await observer.next(v => v.t === 'error')).code, 'invalid_lease');
    observer.send({ t: 'claim', deviceId: 'car2' });
    assert.equal((await observer.next(v => v.t === 'error')).code, 'forbidden_device');
    observer.send({ t: 'claim', deviceId: 'constructor' });
    assert.equal((await observer.next(v => v.t === 'error')).code, 'forbidden_device');
    car.send({ t: 'tm', spd: 4, owner: 0, marker: 'real-frame' });
    assert.equal((await observer.next(v => v.t === 'tm')).marker, 'real-frame');
    observer.send({ t: 'command', deviceId: 'car1', command: { t: 'stop' } });
    assert.equal((await observer.next(v => v.t === 'error')).code, 'invalid_lease');
  });
  let lease;
  await t.test('唯一操作者租约，动作不在服务器生成，设备ACK绑定租约', async () => {
    driver.send({ t: 'claim', deviceId: 'car1' }); lease = await driver.next(v => v.t === 'lease');
    const admitted = await car.next(v => v.t === 'lease'); assert.equal(admitted.lease, lease.lease);
    rival.send({ t: 'claim', deviceId: 'car1' });
    assert.equal((await rival.next(v => v.t === 'error')).code, 'busy');
    command(driver, lease, car, 1, { t: 'hb' });
    await car.next(v => v.t === 'command' && v.seq === 1);
    command(driver, lease, car, 2, { t: 'drv', x: 0, y: 1, spd: 30 });
    const frame = await car.next(v => v.t === 'command' && v.seq === 2);
    assert.equal(frame.boot, car.boot); assert.equal(frame.link, car.link);
    assert.deepEqual(frame.command, { t: 'drv', x: 0, y: 1, spd: 30 });
    car.send({ t: 'ack', lease: token(), seq: 2, ok: true, message: 'old' });
    car.send({ t: 'ack', lease: lease.lease, seq: 2, ok: true, message: 'accepted' });
    assert.equal((await driver.next(v => v.t === 'ack')).message, 'accepted');
  });
  await t.test('重复序号、旧permit、伪lease和云端本地专用指令全部拒绝', async () => {
    command(driver, lease, car, 2, { t: 'hb' });
    assert.equal((await driver.next(v => v.t === 'error')).code, 'stale_sequence');
    command(driver, lease, car, 3, { t: 'hb' }, { permit: token() });
    assert.equal((await driver.next(v => v.t === 'error')).code, 'permit_expired');
    command(driver, lease, car, 3, { t: 'hb' }, { lease: token() });
    assert.equal((await driver.next(v => v.t === 'error')).code, 'invalid_lease');
    command(driver, lease, car, 3, { t: 'unlock' });
    assert.equal((await driver.next(v => v.t === 'error')).code, 'forbidden_command');
    command(driver, lease, car, 3, { t: 'drv', x: '0', y: 1, spd: 30 });
    assert.equal((await driver.next(v => v.t === 'error')).code, 'bad_command');
    command(driver, lease, car, 3, { t: 'hb' }); await car.next(v => v.t === 'command' && v.seq === 3);
  });
  await t.test('应用心跳停止即结束租约，设备仍在线不能服务器伪续', async () => {
    const ended = await car.next(v => v.t === 'lease_end');
    assert.equal(ended.lease, lease.lease); assert.equal(ended.reason, 'operator_heartbeat_lost');
    assert.equal(car.ws.readyState, WebSocket.OPEN);
    command(driver, lease, car, 4, { t: 'drv', x: 0, y: 1, spd: 30 });
    assert.equal((await driver.next(v => v.t === 'error')).code, 'invalid_lease');
  });
  await t.test('有真实hb可以续租，实际过期旧permit仍拒绝，其他动作不会代替hb', async () => {
    driver.send({ t: 'claim', deviceId: 'car1' }); const freshLease = await driver.next(v => v.t === 'lease');
    await car.next(v => v.t === 'lease');
    const oldPermit = car.permit;
    let seq = 0;
    const applicationHb = setInterval(() => command(driver, freshLease, car, ++seq, { t: 'hb' }), 80);
    timers.push(applicationHb);
    await delay(650); clearInterval(applicationHb);
    command(driver, freshLease, car, ++seq, { t: 'hb' }, { permit: oldPermit });
    assert.equal((await driver.next(v => v.t === 'error')).code, 'permit_expired');
    const motionTraffic = setInterval(() => command(driver, freshLease, car, ++seq,
      { t: 'drv', x: 0, y: 1, spd: 20 }), 50);
    timers.push(motionTraffic);
    const ended = await car.next(v => v.t === 'lease_end'); clearInterval(motionTraffic);
    assert.equal(ended.reason, 'operator_heartbeat_lost');
    assert.equal(ended.lease, freshLease.lease);
    // 丢弃已经真实转发的驾驶更新，仅继续检查后续断线不会生成任何新动作。
    car.messages.splice(0); driver.messages.splice(0);
  });
  await t.test('急停使用现有租约，新租约序号归零；断线不发送stop/auto恢复', async () => {
    driver.send({ t: 'claim', deviceId: 'car1' }); const nextLease = await driver.next(v => v.t === 'lease');
    assert.notEqual(nextLease.lease, lease.lease); await car.next(v => v.t === 'lease');
    command(driver, nextLease, car, 1, { t: 'estop' });
    assert.equal((await car.next(v => v.t === 'command' && v.seq === 1)).command.t, 'estop');
    driver.ws.close(); await driver.closed();
    assert.equal((await car.next(v => v.t === 'lease_end')).reason, 'operator_disconnected');
    assert.equal(car.messages.some(v => v.t === 'command' && ['auto', 'drv', 'stop'].includes(v.command.t)), false);
  });
  await t.test('设备断线时释放租约，离线动作不缓存，重连产生新link', async () => {
    rival.send({ t: 'claim', deviceId: 'car1' }); const rivalLease = await rival.next(v => v.t === 'lease');
    await car.next(v => v.t === 'lease');
    clearInterval(car.timer); car.ws.close(); await car.closed();
    assert.equal((await rival.next(v => v.t === 'lease_end')).reason, 'device_disconnected');
    command(rival, rivalLease, car, 1, { t: 'auto' });
    assert.equal((await rival.next(v => v.t === 'error')).code, 'device_offline');
    const reconnected = await device(); currentCar = reconnected; assert.notEqual(reconnected.link, car.link);
    await delay(50);
    assert.equal(reconnected.messages.some(v => v.t === 'command' || v.t === 'lease'), false);
    command(rival, rivalLease, reconnected, 2, { t: 'drv', x: 0, y: 1, spd: 30 });
    assert.equal((await rival.next(v => v.t === 'error')).code, 'invalid_lease');
  });
  await t.test('超大operator帧关闭连接，主动断开撤销cookie与WS', async () => {
    const oversized = await operator(rivalCookie);
    oversized.ws.send('x'.repeat(2049)); await oversized.closed();
    const result = await req('/api/disconnect', { method: 'POST', origin, cookie: observerCookie, body: {} });
    assert.equal(result.status, 200); await observer.closed();
    assert.equal((await req('/api/me', { cookie: observerCookie })).status, 401);
    assert.match(result.headers['set-cookie'][0], /Max-Age=0/);
    // 实际独立HTTPS服务验证会话到期；无需伪造系统时钟或关闭证书检查。
    const expiryPort = await freePort(), expiryOrigin = `https://127.0.0.1:${expiryPort}`;
    const expiryRelay = createRelay({ publicOrigin: expiryOrigin, tls: { cert, key }, sessionMs: 1200,
      config: { deviceId: 'car1', keySha256: keyDigest(deviceKey), controlKeySha256: keyDigest(controlKey) } });
    await new Promise(resolve => expiryRelay.server.listen(expiryPort, '127.0.0.1', resolve));
    try {
      const connected = await request(expiryOrigin + '/api/connect', { ca: cert, method: 'POST',
        origin: expiryOrigin, body: { controlKey } });
      const expiredCookie = connected.headers['set-cookie'][0].split(';')[0];
      const expiring = await connect(expiryOrigin.replace('https:', 'wss:') + '/ws/operator',
        { ca: cert, origin: expiryOrigin, headers: { Cookie: expiredCookie } });
      clients.push(expiring); await expiring.next(v => v.t === 'welcome'); await expiring.closed();
      assert.equal((await request(expiryOrigin + '/api/me', { ca: cert, cookie: expiredCookie })).status, 401);
      await assert.rejects(connect(expiryOrigin.replace('https:', 'wss:') + '/ws/operator',
        { ca: cert, origin: expiryOrigin, headers: { Cookie: expiredCookie } }), /upgrade 401/);
    } finally { await expiryRelay.close(); }
  });
  await t.test('发送限速超限关闭操作者并撤租，不留下占用', async () => {
    const flooding = await operator(driverCookie);
    flooding.send({ t: 'claim', deviceId: 'car1' }); const activeLease = await flooding.next(v => v.t === 'lease');
    await currentCar.next(v => v.t === 'lease');
    for (let seq = 1; seq <= 42; seq++) command(flooding, activeLease, currentCar, seq, { t: 'hb' });
    await flooding.closed();
    assert.equal((await currentCar.next(v => v.t === 'lease_end')).reason, 'rate_limit');
  });
  await t.test('设备停止签发许可，仍有TCP连接也会标离线', async () => {
    clearInterval(currentCar.timer);
    const lastObserver = await operator(rivalCookie);
    assert.equal(lastObserver.welcome.device.online, true);
    const offline = await lastObserver.next(v => v.t === 'device' && v.deviceId === 'car1' && !v.online, 3000);
    assert.equal(offline.permit, ''); assert.equal(offline.link, '');
    await currentCar.closed();
  });
  assert.equal(process.env.NODE_TLS_REJECT_UNAUTHORIZED, undefined, '测试不能禁用全局TLS验证');
  await access(certFile);
});
