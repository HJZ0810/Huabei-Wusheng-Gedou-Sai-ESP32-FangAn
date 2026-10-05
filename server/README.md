# CombatBot V4 单车云端中继

车辆在 STA 联网且云配置启用后主动建立 WSS；普通 AP 模式继续使用车端网页。公网入口为 `https://combatbot.luo-jin-ai.com/`。专属控制链接在浏览器中自动连接这一台车辆，不需要输入账户、口令或选择车辆。

## 专属链接和首次配置

服务采用两把独立的256位随机密钥：车端密钥用于 ESP32 主动连接，控制密钥用于打开网页。持有专属控制链接即可取得控制权限，因此只向实际操作者提供该链接。配置只保存 SHA256 摘要，服务不打印完整链接、Cookie 或任何密钥。

在仓库外创建受限目录，再生成新配置：

```sh
export COMBATBOT_DEVICE_ID='combatbot-01'
node scripts/create-config.mjs /etc/combatbot/config.private.json
```

生成器排他创建以下文件，不覆盖已有配置：

| 文件 | 用途 |
| --- | --- |
| `config.private.json` | 单车编号、车端密钥摘要、网页控制密钥摘要 |
| `config.private.json.device-key` | 只填写到这台车的本地云配置 |
| `config.private.json.control-key` | 组成 `https://combatbot.luo-jin-ai.com/#control=<密钥>` 专属链接 |

Linux 创建权限为0600；Windows 应用 ACL 限制读取。所有文件保存在仓库外，不上传 Git、发布包或公开文档。配置结构如下，占位值不能直接启动：

```json
{
  "deviceId": "combatbot-01",
  "keySha256": "64位十六进制SHA256摘要",
  "controlKeySha256": "另一把独立随机密钥的64位十六进制SHA256摘要"
}
```

浏览器读取 `#control` 后立即移除地址栏片段，通过同源 `POST /api/connect` 将控制密钥换为 HttpOnly、Secure、SameSite=Strict 会话 Cookie。片段不会随网页 HTTP 请求发送；网页不将密钥放入本地存储、查询参数、WebSocket 地址或日志。会话默认8小时，服务重启后失效；再次打开专属链接即可重连。公开首页可以查看界面，缺少有效会话无法取得遥测或发送控制指令。

链接泄漏时生成新控制密钥、替换配置中的摘要并重启独立服务，使旧链接和现有内存会话失效；车辆密钥无需同时更换。

## 安装和运行

要求 Node.js 22 LTS，执行 `npm ci --ignore-scripts`，再执行 `npm test`。依赖锁定为 `ws 8.22.0`。真实 TLS 集成测试需要 OpenSSL，可通过 `OPENSSL_BIN` 指定；Windows 尝试 Git 自带的 OpenSSL。

| 环境变量 | 含义 |
| --- | --- |
| `COMBATBOT_CONFIG` | 仓库外私有配置的绝对路径，必填 |
| `COMBATBOT_PUBLIC_ORIGIN` | `https://combatbot.luo-jin-ai.com`，不含路径，必填 |
| `COMBATBOT_BASE_PATH` | 当前根站点为空字符串 |
| `COMBATBOT_HOST` | 默认 `127.0.0.1` |
| `COMBATBOT_PORT` | 默认8090 |
| `COMBATBOT_TLS_CERT` / `COMBATBOT_TLS_KEY` | Node 直接提供 TLS 时同时设置；Nginx 终止 TLS 时留空 |

`npm start` 不自动读取 `.env`。可使用 `node --env-file=/etc/combatbot/relay.env src/main.mjs`，或由独立 systemd 服务传入环境。Node 不提供 TLS 时只能绑定回环地址，公网 HTTPS/WSS 由 Nginx 提供。

服务读取同一工程中的 `combatbot/web/index.html`，部署必须同时携带 `server/` 和该网页路径。单进程运行；会话和控制租约存于内存，不使用 PM2 cluster。部署示例位于 `deploy/`，实际改动需先备份并保留同机其他业务路由。

## HTTP 合同

所有返回均为 `Cache-Control: no-store`。浏览器的连接、断开请求以及 WS upgrade 校验 HTTPS Origin；密钥不通过 URL 查询传递。

| 路径 | 请求与结果 |
| --- | --- |
| `GET /api/info` | 公共描述 `{ok,mode:"cloud",version:"4.0.0",basePath:"",wsOperator:"/ws/operator",singleDevice:true,deviceId}` |
| `POST /api/connect` | JSON `{controlKey}`；成功返回 `{ok:true,device}` 并设置会话 Cookie |
| `GET /api/me` | 有效会话返回 `{ok:true,device}`；缺失或过期返回401 |
| `POST /api/disconnect` | 撤销当前会话、关闭该会话 WS、撤销其人工控制租约并清 Cookie |
| `GET /` | 当前控制 HTML |
| `/ws/operator` | 有效会话及正确 Origin 的浏览器 WSS |
| `/ws/device` | 设备编号及独立车端密钥匹配的主动 WSS |

设备通过 `X-CombatBot-Device` 与 `X-CombatBot-Key` 请求头认证。公开信息、会话描述及遥测均不包含密钥摘要或明文密钥。云端不提供配置写入、WiFi 凭据读取、解锁、标定或重启 API，这些继续在本地操作。

## 自动绑定单车与控制租约

浏览器连接即收到 `{t:"welcome",device}`，并自动订阅唯一车辆的遥测和许可，不发送选车指令。设备状态为 `{deviceId,online,boot,link,permit,permitTtlMs,owner}`；owner 为 `"remote"` 或 null，表示是否已有远端控制租约，与车端电机 owner 编号不同。

1. 浏览器发送 `{t:"claim",deviceId}`。车辆在线、有有效 permit 且未被其他页面占用时，服务回复 `{t:"lease",deviceId,lease,boot,link,permit,permitTtlMs,...}`，并向车端发送 `{t:"lease",boot,link,lease,state:"claimed"}`。
2. 所有动作含停止和急停均要求该连接持有有效租约。第二个页面可查看遥测，无法在未取得租约时运动、停止或抢占另一页面。
3. 浏览器每200ms发送真实应用 `hb`。租约只代表远端会话有效，车端本地控制权、安全锁及参数检查仍可拒绝动作。
4. 动作按以下格式发送，seq 从1单调递增；permit 使用设备最新签发的短期 token：

```json
{
  "t": "command",
  "deviceId": "combatbot-01",
  "lease": "32位十六进制租约",
  "seq": 1,
  "permit": "设备签发的短期token",
  "command": { "t": "drv", "x": 0, "y": 1, "spd": 30 }
}
```

允许 `drv/move/turn/servo/stop/estop/hb/pose/goto/auto/climb/takeover`。中继补上真实车端 boot/link，不接受浏览器伪造代次；物理单位和范围最终由固件验证。设备首次发送 `{t:"device_hello",boot,link,permit,permitTtlMs:600}`，收到 `{t:"ready",boot,link}` 后约每250ms签发新 permit。boot/link/permit 为32~64位十六进制，lease 为32位。

设备 ACK 必须携带 `{t:"ack",lease,seq,ok,message}`，避免新租约序号归零后配对旧 ACK。真实 `tm/done` 自动广播到所有已授权连接，附唯一 deviceId。主动释放为 `{t:"release",deviceId,lease}`；撤销通知为 `{t:"lease_end",deviceId,lease,reason}`。断线、会话过期、缺心跳、拥塞或限流都会撤销人工租约；设备侧连接已绑定单车，因此 lease_end 无需 deviceId。

## 时效与失联

只有实际收到该租约的 `command.t === "hb"` 才刷新人工租约，默认1200ms超时。WS ping/pong、车辆在线以及持续 drv 不替代心跳。许可最多保留四槽，同 token 不延长期限；重复序号、旧 lease、过期 permit、错误车号、无效数值、未知命令及离线动作均拒绝，不排队等待上线。

车端继续使用单调时钟检查许可原始截止时间，服务器接收时刻不能替代真实签发时刻。车辆2秒未签发新 permit 即标离线，重连产生新 boot/link/lease，不恢复旧人工动作。

浏览器关闭或云断线仅结束人工 owner；已明确启动的自动格斗、导航或登台任务继续由车端有界执行，防跌落持续运行。服务不伪造全局 stop，也不在重连时自动发 auto。

负载上限：一车一设备连接、一个远端控制租约、64个浏览器 WS、256个会话。浏览器帧最大2048字节、每连接每秒40包；设备帧最大16KB、每秒120包。发送缓存超过32KB时丢弃可替代遥测，控制拥塞关闭连接并撤租。连接换 Cookie 每来源地址每分钟10次、最多4个并行请求；服务不信任伪造的 X-Forwarded-For，代理部署应在公网入口另做真实来源限流。

## 验证证据和边界

单车版本已用官方 Node 22.23.3 通过14项真实 HTTPS/HTTP/WSS 测试（含父测试），覆盖缺失/错误控制密钥、密钥用途隔离、Cookie/Origin、自动绑定、错误车号、设备连接排他、独占控制租约、ACK 绑定、重复 seq、过期 permit、真实 hb 与持续动作的区别、断线撤租、离线不排队、重连不恢复、超大帧、限流撤租以及停发 permit 标离线。测试临时自签证书由客户端明确作为 CA 信任，没有关闭 TLS 校验。

公网部署可执行 `node scripts/public-smoke.mjs <仓库外受限JSON>`。JSON 字段为 `{url,controlKey,deviceId,deviceKey}`，入口 URL 不包含查询或片段。脚本首先确认真实车辆离线，再使用协议替身检查实际 HTTPS/WSS、Cookie、租约、转发、ACK、遥测和断线撤销；不输出密钥。公网检查结果由实际执行日志记录，不用本机测试冒充。

上述测试不证明 ESP32 实际 WSS 握手、堆峰值、无线延迟、电机控制、登台或防跌落效果。这些须安装车辆后按实车验收清单完成。
