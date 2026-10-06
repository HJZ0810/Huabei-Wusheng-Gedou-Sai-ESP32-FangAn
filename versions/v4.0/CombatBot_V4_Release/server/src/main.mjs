/** @file main.mjs @brief 从受限单车配置启动中继；不打印控制链接或设备凭据。 */
import { readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { createRelay } from './relay.mjs';

const env = process.env;
if (!env.COMBATBOT_CONFIG || !env.COMBATBOT_PUBLIC_ORIGIN)
  throw new Error('请设置 COMBATBOT_CONFIG 与 COMBATBOT_PUBLIC_ORIGIN');
const config = JSON.parse(await readFile(env.COMBATBOT_CONFIG, 'utf8'));
const tls = env.COMBATBOT_TLS_CERT && env.COMBATBOT_TLS_KEY ? {
  cert: await readFile(env.COMBATBOT_TLS_CERT), key: await readFile(env.COMBATBOT_TLS_KEY),
} : undefined;
if (Boolean(env.COMBATBOT_TLS_CERT) !== Boolean(env.COMBATBOT_TLS_KEY))
  throw new Error('TLS 证书与密钥必须同时配置');
const host = env.COMBATBOT_HOST || '127.0.0.1';
if (!tls && !['127.0.0.1', '::1', 'localhost'].includes(host))
  throw new Error('未启用 Node TLS 时只能绑定回环地址，由 HTTPS 反向代理转发');
const port = Number(env.COMBATBOT_PORT || 8090);
if (!Number.isInteger(port) || port < 1 || port > 65535) throw new Error('端口配置无效');
const relay = createRelay({ config, tls, basePath: env.COMBATBOT_BASE_PATH || '',
  publicOrigin: env.COMBATBOT_PUBLIC_ORIGIN,
  webFile: fileURLToPath(new URL('../../combatbot/web/index.html', import.meta.url)),
});
relay.server.listen(port, host, () => console.log(`CombatBot V4 relay listening ${host}:${port}${relay.basePath}/`));
let shuttingDown = false;
for (const signal of ['SIGINT', 'SIGTERM']) process.on(signal, async () => {
  if (shuttingDown) return;
  shuttingDown = true;
  await relay.close(); process.exit(0);
});
