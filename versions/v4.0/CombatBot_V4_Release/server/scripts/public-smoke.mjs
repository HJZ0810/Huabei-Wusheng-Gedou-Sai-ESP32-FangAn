/**
 * @file public-smoke.mjs
 * @brief 通过真实公网 HTTPS/WSS 校验部署入口；只允许在目标车辆离线时使用协议替身。
 * @details 凭据从仓库外受限文件读取，不输出 Cookie、控制密钥、设备密钥或租约。
 *          此检查不代表 ESP32 的握手、传感器或电机已经通过实车验收。
 */
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { randomBytes } from 'node:crypto';
import { WebSocket } from 'ws';

const privateFile = process.argv[2];
if (!privateFile) throw new Error('参数须为仓库外的受限部署凭据 JSON 文件');
const credentials = JSON.parse(await readFile(privateFile, 'utf8'));
const entry = new URL(credentials.url);
assert(entry.protocol === 'https:' && !entry.username && !entry.password && !entry.search && !entry.hash,
  '公网测试入口须为不含凭据、查询或片段的HTTPS地址');
const base = new URL(credentials.url).href.replace(/\/$/, '');
const origin = new URL(base).origin;
assert.equal(new URL(base).protocol, 'https:');
const token = () => randomBytes(16).toString('hex');
const sockets = [];
let cookie, timer;
async function request(suffix, options = {}) {
  return fetch(base + suffix, { redirect: 'manual', ...options,
    headers: { ...(cookie ? { Cookie: cookie } : {}), ...options.headers } });
}
async function connect(suffix, options = {}) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(base.replace('https:', 'wss:') + suffix, options);
    const queued = [], pending = [];
    sockets.push(ws);
    ws.on('error', reject);
    ws.on('unexpected-response', (_request, res) => {
      res.resume(); ws.terminate(); reject(new Error(`WS upgrade HTTP ${res.statusCode}`));
    });
    ws.on('message', buffer => {
      const value = JSON.parse(buffer.toString());
      const at = pending.findIndex(item => item.matches(value));
      if (at >= 0) { const item = pending.splice(at, 1)[0]; clearTimeout(item.timer); item.resolve(value); }
      else queued.push(value);
    });
    ws.once('open', () => resolve({ ws, send: data => ws.send(JSON.stringify(data)),
      next(matches) {
        const at = queued.findIndex(matches);
        if (at >= 0) return Promise.resolve(queued.splice(at, 1)[0]);
        return new Promise((done, fail) => {
          const item = { matches, resolve: done, timer: null };
          item.timer = setTimeout(() => { const at = pending.indexOf(item); if (at >= 0) pending.splice(at, 1);
            fail(new Error('公网协议等待超时')); }, 5000);
          pending.push(item);
        });
      }
    }));
  });
}
try {
  const info = await request('/api/info');
  assert.equal(info.status, 200);
  const description = await info.json();
  assert.equal(description.version, '4.0.0'); assert.equal(description.singleDevice, true);
  assert.equal(description.deviceId, credentials.deviceId);
  const page = await request('/'); assert.equal(page.status, 200);
  const html = await page.text();
  const source = await readFile(new URL('../../combatbot/web/index.html', import.meta.url), 'utf8');
  assert.equal(html, source, '公网页面必须是发布网页原文，不能受已有 SPA 注入影响');
  assert.equal((await request('/api/me')).status, 401);
  assert.equal((await request('/api/connect', { method: 'POST', headers: { Origin: 'https://invalid.example',
    'Content-Type': 'application/json' }, body: '{}' })).status, 403);
  console.log('PASS HTTPS certificate validation, exact page, info, anonymous and Origin boundaries');
  const connected = await request('/api/connect', { method: 'POST', headers: { Origin: origin,
    'Content-Type': 'application/json' }, body: JSON.stringify({ controlKey: credentials.controlKey }) });
  assert.equal(connected.status, 200);
  const setCookie = connected.headers.getSetCookie()[0];
  assert(setCookie && /HttpOnly/i.test(setCookie) && /Secure/i.test(setCookie) && /SameSite=Strict/i.test(setCookie),
    '控制会话Cookie安全属性缺失');
  cookie = setCookie.split(';')[0];
  const operator = await connect('/ws/operator', { origin, headers: { Cookie: cookie } });
  const welcome = await operator.next(v => v.t === 'welcome');
  const target = welcome.device;
  assert.equal(target.deviceId, credentials.deviceId);
  assert(target && !target.online && !target.owner, '目标车辆在线或已有人控制，禁止替身覆盖连接');
  const device = await connect('/ws/device', { headers: {
    'X-CombatBot-Device': credentials.deviceId, 'X-CombatBot-Key': credentials.deviceKey } });
  const boot = token(), link = token(); let permit = token();
  device.send({ t: 'device_hello', boot, link, permit, permitTtlMs: 600 });
  await device.next(v => v.t === 'ready' && v.boot === boot && v.link === link);
  timer = setInterval(() => { permit = token(); device.send({ t: 'permit', boot, link, permit, permitTtlMs: 600 }); }, 200);
  await operator.next(v => v.t === 'device' && v.online);
  operator.send({ t: 'claim', deviceId: credentials.deviceId });
  const lease = await operator.next(v => v.t === 'lease');
  const admitted = await device.next(v => v.t === 'lease'); assert(admitted.lease === lease.lease, '设备与浏览器租约不一致');
  console.log('PASS private control link, Secure cookie, authenticated operator/device WSS, lease');
  const command = { t: 'command', deviceId: credentials.deviceId, lease: lease.lease, seq: 1,
    permit, command: { t: 'hb' } };
  operator.send(command);
  const forwarded = await device.next(v => v.t === 'command');
  assert.equal(forwarded.boot, boot); assert.equal(forwarded.link, link);
  assert(forwarded.lease === lease.lease, '转发租约不一致'); assert.equal(forwarded.command.t, 'hb');
  device.send({ t: 'ack', lease: lease.lease, seq: 1, ok: true, message: 'protocol substitute' });
  await operator.next(v => v.t === 'ack' && v.seq === 1 && v.ok);
  operator.send(command);
  await operator.next(v => (v.t === 'error' || v.t === 'ack') && v.ok !== true);
  device.send({ t: 'tm', testing: true, marker: 'public-protocol-substitute' });
  await operator.next(v => v.t === 'tm' && v.marker === 'public-protocol-substitute');
  operator.ws.close();
  const ended = await device.next(v => v.t === 'lease_end');
  assert(ended.lease === lease.lease, '撤销租约不一致');
  console.log('PASS command/ACK/telemetry relay, duplicate sequence rejection, disconnect lease_end');
  clearInterval(timer); timer = undefined;
  device.ws.close();
  const disconnected = await request('/api/disconnect', { method: 'POST', headers: { Origin: origin } });
  assert.equal(disconnected.status, 200);
  cookie = undefined;
  console.log('PASS disconnect; protocol substitute complete, no physical vehicle tested');
} finally {
  clearInterval(timer);
  sockets.forEach(ws => ws.terminate());
}
