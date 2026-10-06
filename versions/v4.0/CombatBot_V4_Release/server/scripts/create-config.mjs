/**
 * @file create-config.mjs
 * @brief 排他生成单车配置及两把独立随机密钥；不输出密钥或覆盖已有文件。
 */
import { randomBytes } from 'node:crypto';
import { writeFile } from 'node:fs/promises';
import { keyDigest } from '../src/auth.mjs';

const output = process.argv[2];
const deviceId = process.env.COMBATBOT_DEVICE_ID || 'combatbot-01';
if (!output || !/^[A-Za-z0-9_-]{1,48}$/.test(deviceId))
  throw new Error('参数须为新配置路径；COMBATBOT_DEVICE_ID 为1~48位ASCII设备编号');
const deviceKey = randomBytes(32).toString('hex');
const controlKey = randomBytes(32).toString('hex');
const config = { deviceId, keySha256: keyDigest(deviceKey), controlKeySha256: keyDigest(controlKey) };
// wx 确保已有配置和凭据不会被替换。若中途发生磁盘错误，保留已写入文件供人工检查。
await writeFile(output, JSON.stringify(config, null, 2) + '\n', { flag: 'wx', mode: 0o600 });
await writeFile(output + '.device-key', deviceKey + '\n', { flag: 'wx', mode: 0o600 });
await writeFile(output + '.control-key', controlKey + '\n', { flag: 'wx', mode: 0o600 });
console.log(`配置已写入 ${output}；车端密钥与网页控制密钥分别保存在 .device-key 和 .control-key 文件。`);
