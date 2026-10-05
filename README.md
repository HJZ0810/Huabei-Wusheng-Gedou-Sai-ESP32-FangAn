# README · ESP32-S3 四驱格斗车无线控制系统

固件 + 网页一体化工程：ESP32-S3 建立常驻 AP、可通过 mDNS 访问内嵌网页，
支持「连续实时控制」与「定点精准控制」两种模式，所有参数网页可调并持久化到 NVS。

作者：HJZ

许可证：[MIT](LICENSE)

## 1. 目录结构

> **所有源文件与 `.ino` 平铺在同一目录**（不再有 `src/` 子目录）。
> 这是刻意为之：Arduino IDE 只把「与 .ino 同级」的 `.h/.cpp` 列为标签页，
> 放到 `src/` 里虽然能编译，但打开 `.ino` 时看不到其它模块，无法跳转阅读。

```
CombatBot_ESP32S3/
├─ CombatBot_ESP32S3.ino    Arduino 入口（setup/loop），Arduino IDE 直接打开
├─ platformio.ini           PlatformIO 工程配置（推荐）
├─ cfg.h/.cpp               配置结构体 + NVS 持久化 + JSON 导入导出
├─ motor.h/.cpp             4×JGB37-3625：LEDC 调速 + 方向 IO + FG 带符号计数 + 舵机
├─ pid.h                    PID 控制器 + 梯形速度规划器
├─ kinematics.h/.cpp        运动学换算（脉冲↔厘米↔角度，含 K_odo / K_turn）
├─ imu.h/.cpp               MPU6050 偏航积分 + ADXL345 姿态/冲击 + 互补滤波
├─ sensors.h/.cpp           CD74HC4067（红外×6 + 电池）+ MCP23017（灰度×4 + E18×3）
├─ safety.h/.cpp            心跳看门狗 / 急停 / 低压 / 堵转 / 边缘保护
├─ robot.h/.cpp             模式状态机 + 三级闭环（FreeRTOS 任务，core0，200Hz）
├─ wifi_mgr.h/.cpp          AP 常驻 + STA 可选状态机 + mDNS + 强制门户
├─ web.h/.cpp               HTTP + WebSocket + REST
├─ web_ui.h                 内嵌单文件网页（改 UI 只改这里）
├─ docs/
│  ├─ hardware/       接线参考
│  ├─ verification/   装机与验收清单
│  ├─ design/         设计与参数依据
│  └─ archive/        历史需求材料
└─ README.md
```

项目文档入口：[docs/README.md](docs/README.md)。固件源码继续放在项目根目录，以兼容 Arduino IDE 与当前 PlatformIO 配置。

> 打开 `CombatBot_ESP32S3.ino` 后，Arduino IDE 顶部会出现
> `cfg / motor / pid / kinematics / imu / sensors / safety / robot / wifi_mgr / web / web_ui`
> 等标签页，可直接点击跳转；PlatformIO 侧由 `build_src_filter = +<*.ino> +<*.cpp>` 扫描同一目录。

## 2. 编译与烧录

### 方式 A：PlatformIO（推荐）

```bash
cd CombatBot_ESP32S3
pio run -t upload && pio device monitor
```

首次会自动下载依赖（`platformio.ini` 的 `lib_deps`）：
ESPAsyncWebServer(me-no-dev) + AsyncTCP(me-no-dev)、ArduinoJson、
Adafruit MPU6050 / ADXL345 / Unified Sensor / MCP23017。

> 原项目说明记录曾在 **espressif32 6.9.0 / Arduino core 2.0.17** 下做过语法与类型校验；该记录未在本次整理中重新验证。
> 若你用的是 Arduino core 3.x，请把 `lib_deps` 前两行换成 mathieucarbou 的
> ESPAsyncWebServer v3 + AsyncTCP v3（`platformio.ini` 里已写好注释行）。

> 若你的板子不是 N16R8，把 `platformio.ini` 里 `board_build.flash_size` 与
> `board_build.partitions` 改成对应值（N8R8 → `8MB` / `default_8MB.csv`）。

### 方式 B：Arduino IDE

1. 安装 ESP32 支持（`https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`），选择 **ESP32S3 Dev Module**。
2. 工具菜单：USB CDC On Boot=Enabled、Flash Mode=QIO、PSRAM=OPI、Flash Size=16MB。
3. 库管理器搜索安装：`ESPAsyncWebServer`(me-no-dev)、`AsyncTCP`(me-no-dev)、
   `ArduinoJson`(v7)、`Adafruit MPU6050`、`Adafruit ADXL345`、`Adafruit Unified Sensor`、
   `Adafruit MCP23017`。
4. 打开 `CombatBot_ESP32S3.ino` 编译上传——同级的所有 `.h/.cpp` 会自动显示为标签页并一起编译。
   （若你之前见过 `src/` 版本：本工程已把模块全部平铺到根目录，无需再手动搬运。）

### 依赖版本坑

- **arduino-esp32 3.x 的 LEDC 接口是按引脚的**（`ledcAttach/ledcWrite(pin,...)`），
  2.x 是按通道的。`motor.cpp` 顶层有 `LEDC_ATTACH / LEDC_WRITE` 兼容宏自动切换，无需手改。
- ESPAsyncWebServer v3 若报 `onEvent` 签名不匹配，改用 me-no-dev 原版 1.2.x，或按库内
  `AwsEventHandler` 定义微调参数类型（其余 API 一致）。

## 3. 首次使用流程

1. 上电 → 串口打印 `AP CombatBot-AP` 与 IP `192.168.4.1`。
2. 手机/电脑连 WiFi `CombatBot-AP`，密码 `12345678`；连上会自动弹控制页（强制门户）。这是当前源码中的公开开发默认值，实际部署前请在设置页修改。
   或手动访问 `http://combatbot.local`（Android 解析不稳时用 `http://192.168.4.1`，页面右上角 ▣ 可出二维码）。
3. 打开页面 → 先做【设置】页的三步标定（见下）。
4. 若需接入家里路由器：【设置】→ 网络 → 填 STA SSID/密码 → 保存 → 重启。
   STA 连上后顶栏徽章变 `AP+STA`，两种模式都可访问 `combatbot.local`。

## 4. 标定流程（决定精度上限，务必按顺序做一次）

### ① IMU 零偏
车静止放置 → 设置页【标定】→「标定零偏」→ 采样 3 秒自动写入 NVS。
> MPU6050 零偏随温度漂移，长时间比赛前建议重做；运行中检测到静止会自动微调零偏。

### ② 行程标定（解算 K_odo，单位=脉冲/厘米）
1. 地面量一条 100cm 直线，做起点/终点标记。
2. 「开始行走」→ 小车自动走完（默认 100cm，可在 `calDist` 改）。
3. 用尺量**实际走了多少**（如 103.5cm）→ 填入「实测距离」→「解算 K_odo」。
4. 固件计算 `K_odo = 总脉冲数 / 实测距离`，写入 NVS。
> 这一步直接绕开"FG 每转到底几个脉冲"的不确定性，是最关键的一步。

### ③ 转向标定（解算 K_turn）
1. 「开始转向」→ 小车原地转 360°（按当前 K_turn 执行）。
2. 观察**实际停下的角度**（如只转了 342°）→ 填入「实测角度」→「解算 K_turn」。
3. 固件计算 `K_turn_new = K_turn_old × 360 / 实测角度`。
4. 重复 1~3 一到两次即可收敛。驼峰轮高摩擦，这一步**不能省**，否则 90° 会变成 80° 左右。

## 5. 通信协议

### WebSocket `/ws`

上行：
```jsonc
{"t":"hb"}                        // 心跳（连续模式每 200ms）
{"t":"drv","x":0.35,"y":0.80,"spd":60}   // 连续驱动，spd 为最大速度上限百分比
{"t":"stop"}                      {"t":"estop"}   {"t":"unlock"}
{"t":"move","dist":30.0,"spd":0}  {"t":"turn","deg":-90}
{"t":"servo","pos":1}             // 0=放铲 1=举铲
{"t":"cal","mode":"odo|turn|imu"} // 开始标定
{"t":"cal","mode":"odo","actual":103.5}  // 回填实测值解算系数
```

下行（15Hz 遥测）：
```jsonc
{"t":"tm","rpm":[120,118,121,119],"spd":45.2,"yaw":12.3,"odo":320,"bat":24.6,
 "ir":[80,65,150,150,150,150],"gr":[1,1,0,0],"e18":[0,0,0],"acc":[0.02,-0.01,1.00],
 "net":"AP+STA","rssi":-55,"st":1,"pg":0.42,"es":0,"lb":0,"cal":0,"sv":1500,"imu":1}
{"t":"done","type":"move","target":30,"actual":29.4,"err":-0.6}
{"t":"alert","code":2,"msg":"堵转保护停机"}
```

### REST

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/` | 内嵌网页 |
| GET | `/api/config` | 读取全部配置 JSON |
| POST | `/api/config` | 保存配置（含 `{"__reset":1}` 恢复出厂） |
| POST | `/api/action` | `{"act":"move","dist":30}` / `turn` / `stop` / `servo` / `estop` / `unlock` |
| POST | `/api/calibrate` | `{"mode":"odo","actual":103.5}` |
| GET | `/api/status` | IP / 网络模式 / mDNS / RSSI |
| POST | `/api/reboot` | 重启 |

## 6. 故障排查

| 现象 | 原因 / 处理 |
|---|---|
| 电机一动 ESP32 就重启 | 电源未分离或去耦不足：检查 470µF 电容、星型接地、必要时 ESP32 单独一路 DC-DC |
| 里程只增不减（后退也变多） | FG 方向符号问题：确认 `motorSetNorm` 里 `s_dir[i]` 与下发指令符号一致；或该轮勾「反转」 |
| 完全测不到转速 | FG 未分压烧毁 / 未接；或 PPR 与实际不符 → 走行程标定 |
| 速度上不去 | 蓝线调速只有 3.3V → 检查电平转换器与 RC 滤波；或 PWM 死区过大 |
| 走直线跑偏 | 调【PID】航向环 Kp；左右轮机械差异大时调【电机】`balLR` |
| 转角总是偏小 | K_turn 偏小 → 重做转向标定；同时确认 α 融合系数（打滑严重可提到 0.95） |
| Yaw 一直漂移 | 未做零偏标定，或 MPU6050 装反 → 勾「陀螺 Z 轴取反」 |
| `combatbot.local` 打不开 | Android mDNS 解析不稳 → 用顶栏 IP 直连或扫码 |
| STA 连不上但网页能开 | 正常设计：AP 永不关闭。检查 SSID/密码，15s 会自动重试 |
| 红外读数全是 200 或乱跳 | 4067 地址脚接错 / 未共地；确认只用 ADC1(GPIO4) |
| 编译报 `ledcWrite` 参数错 | arduino-esp32 3.x 需要按引脚调用，兼容宏已处理；确认 `ESP_ARDUINO_VERSION_MAJOR` 可见 |

## 7. 安全须知

- 上电默认 PWM=0、舵机回中，无任何自启动。
- 松开摇杆 / 关页面 / 断网 / 心跳超时（默认 1000ms）→ 立即停车。
- 急停按钮常驻顶栏，单击锁定，再点解锁。
- 电池低于 `batLow`(21.5V) 限速 50%，低于 `batCrit`(20.5V) 停机。
- 任一电机 PWM 高但 FG 无脉冲超 1.5s → 判堵转停机。
- 调试时建议先架起车体（轮子离地）验证转向与转速方向，再落地跑。
