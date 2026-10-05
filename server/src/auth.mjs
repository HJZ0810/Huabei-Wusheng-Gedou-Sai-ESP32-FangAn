/**
 * @file    auth.mjs
 * @brief   单车控制链接与车端凭据的摘要校验、随机会话基础设施。
 * @note    两把独立256位随机密钥分别用于网页和车辆；服务配置仅保存SHA256。
 *          不建立账户、口令或多车授权表；重启后内存会话自然失效。
 */
import { randomBytes, timingSafeEqual, createHash } from 'node:crypto';

export const randomToken = () => randomBytes(16).toString('hex');
export const keyDigest = key => createHash('sha256').update(key).digest('hex');

export function verifySecretKey(key, digest) {
  if (typeof key !== 'string' || !/^[a-f0-9]{64}$/.test(key) ||
      typeof digest !== 'string' || !/^[a-f0-9]{64}$/.test(digest)) return false;
  return timingSafeEqual(Buffer.from(keyDigest(key), 'hex'), Buffer.from(digest, 'hex'));
}

export function validateConfig(config) {
  if (!config || typeof config !== 'object' || Array.isArray(config) ||
      Object.keys(config).sort().join(',') !== 'controlKeySha256,deviceId,keySha256' ||
      typeof config.deviceId !== 'string' || typeof config.keySha256 !== 'string' ||
      typeof config.controlKeySha256 !== 'string' ||
      !/^[A-Za-z0-9_-]{1,48}$/.test(config.deviceId) ||
      !/^[a-f0-9]{64}$/.test(config.keySha256) ||
      !/^[a-f0-9]{64}$/.test(config.controlKeySha256) ||
      config.keySha256 === config.controlKeySha256)
    throw new Error('需要单车编号及两把独立密钥摘要；不接受账户或多车配置');
  return { ...config };
}
