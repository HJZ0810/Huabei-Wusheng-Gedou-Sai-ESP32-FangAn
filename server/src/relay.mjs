/**
 * @file    relay.mjs
 * @brief   连接一台主动上云车辆与持有专属控制链接的浏览器。
 *
 * ============================================================================
 * 控制约定：每车一个远端操作者；服务器不生成运动，不替浏览器续心跳。
 * 失联约定：撤销人工租约，不恢复陈旧动作；已授权自主任务由车端独立判断。
 * 安全边界：本模块的时间检查仅提前筛除，设备单调时钟和控制任务再次验期。
 * ============================================================================
 */
import http from 'node:http';
import https from 'node:https';
import { readFile } from 'node:fs/promises';
import { performance } from 'node:perf_hooks';
import { WebSocket, WebSocketServer } from 'ws';
import { randomToken, validateConfig, verifySecretKey } from './auth.mjs';

const EPOCH = /^[a-f0-9]{32,64}$/;
const LEASE = /^[a-f0-9]{32}$/;
const ALLOWED = new Set(['drv', 'move', 'turn', 'servo', 'stop', 'estop', 'hb',
  'pose', 'goto', 'auto', 'climb', 'takeover']);
const object = value => value !== null && typeof value === 'object' && !Array.isArray(value);
const now = () => performance.now();
function requestPath(req) {
  try { return new URL(req.url, 'https://invalid.local').pathname; } catch { return null; }
}

export function normalizeBasePath(value = '') {
  const base = value === '/' ? '' : String(value).replace(/\/$/, '');
  if (base && !/^\/(?:[A-Za-z0-9_-]+\/)*[A-Za-z0-9_-]+$/.test(base))
    throw new Error('BASE_PATH 必须为简单路径，不包含查询、点段或编码');
  return base;
}

export function createRelay(options) {
  const basePath = normalizeBasePath(options.basePath);
  const origin = new URL(options.publicOrigin).origin;
  if (!origin.startsWith('https://')) throw new Error('公网入口必须使用 HTTPS');
  const registration = validateConfig(options.config);
  const sessionMs = options.sessionMs ?? 8 * 60 * 60 * 1000;
  const leaseTimeoutMs = options.leaseTimeoutMs ?? 1200;
  if (leaseTimeoutMs < 200 || leaseTimeoutMs > 3000) throw new Error('远控租约超时必须为200~3000ms');
  const device = {
    deviceId: registration.deviceId, ws: null, boot: '', link: '', permits: new Map(), ready: false,
    telemetry: null, lease: null, permitAt: 0,
  };
  const operators = new Set();
  const sessions = new Map();
  const connectBuckets = new Map();
  const operatorServer = new WebSocketServer({ noServer: true, maxPayload: 2048, perMessageDeflate: false });
  const deviceServer = new WebSocketServer({ noServer: true, maxPayload: 16384, perMessageDeflate: false });
  let activeConnects = 0;
  const cookieName = 'CombatBotSession';
  const cookiePath = basePath || '/';

  const response = (res, status, body, headers = {}) => {
    res.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8',
      'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff', ...headers });
    res.end(JSON.stringify(body));
  };
  function send(ws, payload, telemetry = false) {
    if (!ws || ws.readyState !== WebSocket.OPEN) return false;
    if (ws.bufferedAmount > 32768) {
      if (!telemetry) ws.close(1013, '连接拥塞，请重新建立控制');
      return false;
    }
    ws.send(JSON.stringify(payload));
    return true;
  }
  const error = (ws, code, message) => send(ws, { t: 'error', code, message });
  function controlSession(req) {
    const raw = String(req.headers.cookie || '').split(';').map(v => v.trim())
      .find(v => v.startsWith(cookieName + '='));
    const token = raw?.slice(cookieName.length + 1);
    const session = sessions.get(token);
    if (!session || session.expires <= Date.now()) return null;
    return { ...session, token };
  }
  function status(device) {
    const permit = [...device.permits].filter(([, deadline]) => deadline > now()).at(-1);
    return { deviceId: device.deviceId, online: device.ready,
      boot: device.boot, link: device.link, permit: permit?.[0] || '',
      permitTtlMs: permit ? Math.max(0, Math.floor(permit[1] - now())) : 0,
      owner: device.lease ? 'remote' : null };
  }
  function broadcast(device, payload, telemetry = false) {
    for (const operator of operators) {
      send(operator.ws, { ...payload, deviceId: device.deviceId }, telemetry);
    }
  }
  function broadcastStatus(device) {
    for (const operator of operators)
      send(operator.ws, { t: 'device', ...status(device) });
  }
  function endLease(device, reason) {
    const lease = device.lease;
    if (!lease) return;
    device.lease = null;
    send(device.ws, { t: 'lease_end', boot: device.boot, link: device.link,
      lease: lease.token, reason });
    send(lease.ws, { t: 'lease_end', deviceId: device.deviceId, lease: lease.token, reason });
    broadcastStatus(device);
  }
  function endOperator(operator, reason) {
    if (device.lease?.ws === operator.ws) endLease(device, reason);
  }
  function offline(device, reason, expectedSocket) {
    if (device.ws !== expectedSocket) return;
    endLease(device, reason);
    device.ws = null; device.ready = false; device.permits.clear(); device.telemetry = null;
    device.boot = ''; device.link = '';
    broadcastStatus(device);
  }

  async function body(req) {
    const chunks = []; let size = 0;
    for await (const chunk of req) {
      size += chunk.length;
      if (size > 2048) { const err = new Error('请求体过大'); err.status = 413; throw err; }
      chunks.push(chunk);
    }
    const parsed = JSON.parse(Buffer.concat(chunks).toString('utf8'));
    if (!object(parsed)) throw new Error('请求必须为 JSON 对象');
    return parsed;
  }
  function rateConnect(req) {
    const ip = req.socket.remoteAddress || 'unknown';
    if (connectBuckets.size >= 1024 && !connectBuckets.has(ip)) return false;
    let bucket = connectBuckets.get(ip);
    if (!bucket || now() - bucket.start > 60000) {
      bucket = { start: now(), count: 0 }; connectBuckets.set(ip, bucket);
    }
    return ++bucket.count <= 10 && activeConnects < 4;
  }
  async function handler(req, res) {
    const path = requestPath(req);
    if (req.method === 'GET' && path === basePath + '/api/info') {
      return response(res, 200, { ok: true, mode: 'cloud', version: '4.0.0', basePath,
        wsOperator: basePath + '/ws/operator', singleDevice: true, deviceId: device.deviceId });
    }
    if (req.method === 'GET' && path === basePath + '/api/me') {
      const session = controlSession(req);
      return session ? response(res, 200, { ok: true, device: status(device) })
        : response(res, 401, { ok: false, message: '请通过专属控制链接连接' });
    }
    if (req.method === 'POST' && [basePath + '/api/connect', basePath + '/api/disconnect'].includes(path)) {
      if (req.headers.origin !== origin) return response(res, 403, { ok: false, message: '请求来源无效' });
      if (path.endsWith('/disconnect')) {
        const session = controlSession(req);
        if (session) {
          sessions.delete(session.token);
          for (const operator of operators) if (operator.sessionToken === session.token) {
            endOperator(operator, 'disconnected'); operator.ws.close(1000, '已断开控制会话');
          }
        }
        return response(res, 200, { ok: true }, {
          'Set-Cookie': `${cookieName}=; Path=${cookiePath}; HttpOnly; Secure; SameSite=Strict; Max-Age=0` });
      }
      if (!rateConnect(req)) return response(res, 429, { ok: false, message: '连接尝试过多，请稍后重试' });
      if (!String(req.headers['content-type'] || '').startsWith('application/json'))
        return response(res, 415, { ok: false, message: '需要 application/json' });
      activeConnects++;
      try {
        const data = await body(req);
        if (Object.keys(data).length !== 1 || !verifySecretKey(data.controlKey, registration.controlKeySha256))
          return response(res, 401, { ok: false, message: '专属控制链接无效' });
        if (sessions.size >= 256) return response(res, 503, { ok: false, message: '会话容量已满' });
        const token = randomToken() + randomToken();
        sessions.set(token, { expires: Date.now() + sessionMs });
        return response(res, 200, { ok: true, device: status(device) }, {
          'Set-Cookie': `${cookieName}=${token}; Path=${cookiePath}; HttpOnly; Secure; SameSite=Strict; Max-Age=${Math.floor(sessionMs / 1000)}` });
      } catch (err) {
        return response(res, err.status || 400, { ok: false, message: '连接请求格式无效' });
      } finally { activeConnects--; }
    }
    if (req.method === 'GET' && [basePath || '/', basePath + '/'].includes(path)) {
      try {
        const content = await readFile(options.webFile);
        res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store',
          'X-Content-Type-Options': 'nosniff', 'Referrer-Policy': 'same-origin',
          'Content-Security-Policy': "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'; base-uri 'self'" });
        return res.end(content);
      } catch { return response(res, 503, { ok: false, message: '控制网页尚未生成' }); }
    }
    return response(res, 404, { ok: false, message: '接口不存在' });
  }
  const server = options.tls ? https.createServer(options.tls, handler) : http.createServer(handler);
  server.headersTimeout = 10000; server.requestTimeout = 10000;
  server.keepAliveTimeout = 5000; server.maxConnections = 256;
  function reject(socket, code = 401) {
    socket.end(`HTTP/1.1 ${code} Rejected\r\nConnection: close\r\nContent-Length: 0\r\n\r\n`);
  }
  server.on('upgrade', (req, socket, head) => {
    const path = requestPath(req);
    if (path === basePath + '/ws/device') {
      const deviceId = req.headers['x-combatbot-device'];
      const key = req.headers['x-combatbot-key'];
      if (deviceId !== registration.deviceId || !verifySecretKey(key, registration.keySha256)) return reject(socket);
      // 现有设备不被第二个连接静默顶替；必须等旧连接失效，防止凭据复用扰动车辆。
      if (device.ws && device.ws.readyState !== WebSocket.CLOSED) return reject(socket, 409);
      return deviceServer.handleUpgrade(req, socket, head, ws => {
        device.ws = ws; device.ready = false;
        const helloTimer = setTimeout(() => ws.close(1008, '设备握手超时'), 5000);
        let quota = { start: now(), count: 0 };
        ws.on('error', () => {});
        ws.on('close', () => { clearTimeout(helloTimer); offline(device, 'device_disconnected', ws); });
        ws.on('message', (data, binary) => {
          if (binary) return ws.close(1008, '仅接受 JSON 文本');
          if (now() - quota.start >= 1000) quota = { start: now(), count: 0 };
          if (++quota.count > 120) return ws.close(1008, '设备发送过快');
          let message;
          try { message = JSON.parse(data.toString()); } catch { return ws.close(1008, 'JSON 无效'); }
          if (!object(message)) return ws.close(1008, '消息必须为对象');
          if (message.t === 'device_hello') {
            if (device.ready || !EPOCH.test(message.boot || '') || !EPOCH.test(message.link || ''))
              return ws.close(1008, '设备代次无效');
            device.boot = message.boot; device.link = message.link;
            if (!registerPermit(device, message)) return ws.close(1008, '设备许可无效');
            device.ready = true; clearTimeout(helloTimer);
            send(ws, { t: 'ready', boot: device.boot, link: device.link });
            broadcastStatus(device);
          } else if (!device.ready) return ws.close(1008, '先完成设备握手');
          else if (message.t === 'permit') {
            if (message.boot !== device.boot || message.link !== device.link || !registerPermit(device, message))
              return ws.close(1008, '设备许可代次无效');
            broadcast(device, { t: 'permit', boot: device.boot, link: device.link,
              permit: message.permit, permitTtlMs: message.permitTtlMs });
          } else if (message.t === 'tm' || message.t === 'done') {
            if (message.t === 'tm') device.telemetry = { ...message, deviceId };
            broadcast(device, message, true);
          } else if (message.t === 'ack') {
            const lease = device.lease;
            if (lease && message.lease === lease.token && Number.isInteger(message.seq) &&
                message.seq > 0 && message.seq <= lease.seq)
              send(lease.ws, { t: 'ack', deviceId, lease: lease.token, seq: message.seq,
                ok: message.ok === true, message: String(message.message || '').slice(0, 240) });
          }
        });
      });
    }
    if (path === basePath + '/ws/operator') {
      const session = controlSession(req);
      if (req.headers.origin !== origin || !session) return reject(socket);
      if (operators.size >= 64) return reject(socket, 503);
      return operatorServer.handleUpgrade(req, socket, head, ws => {
        const operator = { ws, sessionToken: session.token, quota: { start: now(), count: 0 } };
        operators.add(operator);
        ws.on('error', () => {});
        ws.on('close', () => { endOperator(operator, 'operator_disconnected'); operators.delete(operator); });
        send(ws, { t: 'welcome', device: status(device) });
        if (device.telemetry) send(ws, device.telemetry, true);
        ws.on('message', (data, binary) => operatorMessage(operator, data, binary));
      });
    }
    reject(socket, 404);
  });

  function registerPermit(device, message) {
    if (!EPOCH.test(message.permit || '') || !Number.isInteger(message.permitTtlMs) ||
        message.permitTtlMs < 100 || message.permitTtlMs > 1000) return false;
    // 不延长相同 token；只有设备新签发的随机许可建立新期限。
    if (device.permits.has(message.permit)) return false;
    for (const [token, deadline] of device.permits) if (deadline <= now()) device.permits.delete(token);
    device.permits.set(message.permit, now() + message.permitTtlMs);
    device.permitAt = now();
    while (device.permits.size > 4) device.permits.delete(device.permits.keys().next().value);
    return true;
  }
  function operatorMessage(operator, data, binary) {
    const ws = operator.ws;
    if (binary) return ws.close(1008, '仅接受 JSON 文本');
    const session = sessions.get(operator.sessionToken);
    if (!session || session.expires <= Date.now()) {
      endOperator(operator, 'session_expired'); return ws.close(1008, '控制会话已过期');
    }
    if (now() - operator.quota.start >= 1000) operator.quota = { start: now(), count: 0 };
    if (++operator.quota.count > 40) {
      endOperator(operator, 'rate_limit'); return ws.close(1008, '指令发送过快');
    }
    let message;
    try { message = JSON.parse(data.toString()); } catch { return error(ws, 'bad_json', 'JSON 无效'); }
    if (!object(message)) return error(ws, 'bad_message', '消息必须为对象');
    if (message.deviceId !== device.deviceId)
      return error(ws, 'forbidden_device', '控制链接只对应这一台车辆');
    if (message.t === 'release') {
      if (device.lease?.ws !== ws || device.lease.token !== message.lease)
        return error(ws, 'invalid_lease', '控制租约无效');
      return endLease(device, 'released');
    }
    if (!device.ready || device.ws?.readyState !== WebSocket.OPEN)
      return error(ws, 'device_offline', '车辆离线，不缓存动作');
    if (message.t === 'claim') {
      if (device.lease) return error(ws, 'busy', '车辆已有远端操作者');
      const state = status(device);
      if (!state.permit) return error(ws, 'permit_expired', '等待设备刷新许可');
      const token = randomToken();
      device.lease = { token, ws, heartbeatAt: now(), seq: 0 };
      if (!send(device.ws, { t: 'lease', boot: device.boot, link: device.link,
        lease: token, state: 'claimed' })) return endLease(device, 'backpressure');
      send(ws, { t: 'lease', ...state, lease: token });
      return broadcastStatus(device);
    }
    if (message.t !== 'command') return error(ws, 'unknown_message', '未知消息类型');
    const lease = device.lease;
    if (!lease || lease.ws !== ws || !LEASE.test(message.lease || '') || message.lease !== lease.token)
      return error(ws, 'invalid_lease', '控制租约无效');
    if (!Number.isInteger(message.seq) || message.seq <= lease.seq || message.seq > 0xFFFFFFFF)
      return error(ws, 'stale_sequence', '指令序号无效或已使用');
    if (!EPOCH.test(message.permit || '') || (device.permits.get(message.permit) || 0) <= now())
      return error(ws, 'permit_expired', '指令许可已过期');
    if (!object(message.command) || !ALLOWED.has(message.command.t))
      return error(ws, 'forbidden_command', '此动作不允许通过云端执行');
    // 车端再次执行参数和范围验证；这里拒绝嵌套载荷、字符串数字及不可用数值。
    for (const [key, value] of Object.entries(message.command)) {
      if (key === 't' || key === 'floor') { if (typeof value !== 'string' || value.length > 24)
        return error(ws, 'bad_command', '指令字段类型无效'); }
      else if (typeof value !== 'number' || !Number.isFinite(value))
        return error(ws, 'bad_command', '指令数值无效');
    }
    lease.seq = message.seq;
    if (message.command.t === 'hb') lease.heartbeatAt = now();
    if (!send(device.ws, { t: 'command', deviceId: device.deviceId, boot: device.boot,
      link: device.link, lease: lease.token, seq: message.seq, permit: message.permit,
      command: message.command })) endLease(device, 'backpressure');
  }
  const timer = setInterval(() => {
    if (device.ready && now() - device.permitAt > 2000) {
      const socket = device.ws;
      offline(device, 'device_permit_lost', socket); socket?.terminate();
    }
    if (device.lease && now() - device.lease.heartbeatAt > leaseTimeoutMs)
      endLease(device, 'operator_heartbeat_lost');
    for (const [token, session] of sessions) if (session.expires <= Date.now()) {
      sessions.delete(token);
      for (const operator of operators) if (operator.sessionToken === token) {
        endOperator(operator, 'session_expired'); operator.ws.close(1008, '控制会话已过期');
      }
    }
    for (const [ip, bucket] of connectBuckets) if (now() - bucket.start > 60000) connectBuckets.delete(ip);
  }, 50);
  timer.unref();
  async function close() {
    clearInterval(timer);
    endLease(device, 'server_shutdown');
    for (const operator of operators) operator.ws.terminate();
    device.ws?.terminate();
    operatorServer.close(); deviceServer.close();
    await new Promise(resolve => server.close(resolve));
  }
  return { server, close, basePath };
}
