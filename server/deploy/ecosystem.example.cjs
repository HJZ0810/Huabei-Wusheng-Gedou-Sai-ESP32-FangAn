/** PM2 示例：替换工程路径与公开入口，不在文件内放真实口令或设备密钥。 */
module.exports = {
  apps: [{
    name: 'combatbot-v4-relay',
    cwd: '/opt/combatbot/server',
    script: 'src/main.mjs',
    interpreter: '/usr/bin/node',
    instances: 1,
    exec_mode: 'fork',
    max_memory_restart: '256M',
    kill_timeout: 3000,
    env: {
      NODE_ENV: 'production',
      COMBATBOT_CONFIG: '/etc/combatbot/config.private.json',
      COMBATBOT_BASE_PATH: '/combatbot',
      COMBATBOT_PUBLIC_ORIGIN: 'https://robot.example.com',
      COMBATBOT_HOST: '127.0.0.1',
      COMBATBOT_PORT: '8090',
    },
  }],
};
