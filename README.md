# CombatBot V3.0 · ESP32-S3 四驱格斗车控制系统

**连续驾驶、距离与转角控制、在线标定、传感器遥测和离线网页，整合为可直接打开的分文件 Arduino 工程。**

作者：**HJZ** · 许可证：[MIT](LICENSE) · 发布日期：2026-10-05

V3.0 重点修正 V2.0 的运动方向、闭环输出、配置保存及多客户端边界，完善安装、迁移和复现资料。保留 V2.0 已有的急停旁路、控制权仲裁、PCNT、换向等待和输入校验。

**[下载 V3.0 Arduino 完整工程](https://github.com/HJZ0810/Huabei-Wusheng-Gedou-Sai-ESP32-FangAn/releases/download/v3.0/CombatBot_V3.0_Arduino.zip)** · [版本差异与迁移](docs/releases/V3.0.md) · [安装与接线](新手安装与接线.md) · [验证记录](验收与验证.md)

## V3.0 带来什么

| 改进 | 使用者得到的变化 |
|---|---|
| 运动方向与单位一致 | 连续页 0% 为零目标速度；电气反转不颠倒逻辑里程；转角支持累计 360° 和多圈 |
| 闭环边界更完整 | 修正航向纠偏方向；按前馈、补偿后的最终 PWM 处理积分饱和；换向等待不积累积分 |
| 可解释的保护策略 | 边缘、IR、倾倒分别配置；IR 渐进限速，换向时兼顾尚未消退的旧方向运动 |
| 配置失败可见 | 完整校验候选，NVS 写成功后发布；网页处理 HTTP 失败，不把失败显示为成功 |
| 多客户端行为明确 | HTTP 与 WebSocket 用随机令牌绑定；旁观者失焦、关闭或切页不自动打断驾驶者 |
| 更易追溯的交付 | 三个指定依赖、双版本编译、模块回归，以及绑定源码与日志哈希的发布凭据 |

比较基线为 [V2.0 公开提交 fd650ac](https://github.com/HJZ0810/Huabei-Wusheng-Gedou-Sai-ESP32-FangAn/commit/fd650ac14d98aebc9779bf88365e23ad4cc5305f)。V2 的 200Hz 与 V3 的 100Hz 是周期设计变化，不能据此声称响应或精度提升；本版未上板。

## 准备编译

1. 解压完整包，打开 [CombatBotArduino/CombatBotArduino.ino](CombatBotArduino/CombatBotArduino.ino)。保留全部同级 `.cpp/.h`，草图目录名应为 `CombatBotArduino`。
2. Arduino IDE 2.x 安装 **esp32 by Espressif Systems 3.3.12**，选择 **ESP32S3 Dev Module**。
3. 通过“添加 .ZIP 库”安装 [Arduino依赖库](Arduino依赖库/) 中的 **ArduinoJson 7.3.1、ESP32Async/AsyncTCP 3.3.2、ESP32Async/ESPAsyncWebServer 3.6.0**。
4. N16R8 推荐：Flash **16MB**、PSRAM **OPI PSRAM**、Flash Mode **QIO**、Partition **16M Flash (3MB APP/9.9MB FATFS)**、USB CDC **Enabled**、CPU **240MHz**。其他型号按实际硬件选择。
5. 点击“验证”。首次编译不需要 Python 或网页构建工具；菜单及排错见 [安装与接线](新手安装与接线.md)。

PlatformIO 维护工程位于 `combatbot/`，[platformio.ini](combatbot/platformio.ini) 固定平台及依赖版本。编译与上传分开，不使用旧 V2 根目录草图或安装指南。

## 首次使用

启动不自动运动。连接热点 **CombatBot-AP**（开发默认密码 **12345678**），打开 `http://192.168.4.1`；支持 mDNS 的终端可使用 `http://combatbot.local`。实际使用前修改默认密码。AP 常驻，STA 失败每 15 秒重试，网络配置保存后重启生效。

四页为连续、精准、传感器和设置。WASD/方向键只在连续页且未编辑文本时生效；空格急停。距离正值前进、负值后退，转角正值左转。动作均有控制者心跳保护；停止/急停绕普通队列，旧命令不能在停止后继续启动，急停后须显式解锁。

**先架起车体确认方向、反馈和接线，再做 IMU、行程、转向标定。** 默认轮径 72mm、轴距 154mm、轮距 170mm、FG 为 90 个上升沿/输出轴圈，这些是待实测的初值。

### 保护与硬件边界

- **保护总开关 `safetyEnabled` 默认关闭**，用于首次接线调试。边缘/IR/倾倒独立开关默认开启，但总开关启用后才生效；确认电平及方向再开启。心跳与堵转保护不依赖总开关。
- IR 默认 60cm 开始渐进限速、25cm 撤驱动。严格无效策略默认开启：相关方向未采样、过期、超量程或异常时拒绝运动；GP2Y0A02 的有效量程为 20～150cm，超量程不等于无障碍。
- 有效低压读数仍触发 50% 限速。V2 的严重低压 `batCrit` 停机阈值不迁移，不能根据“24V”猜测电池化学体系。
- 停车为 **PWM=0 撤驱动**，不保证主动制动或立即静止。冲击只提示，不自动停车。
- 随机令牌用于连接绑定，**不是账户认证**；能接入设备网络的客户端仍可自行握手。手动停止/急停保持可用，自动离页停止仅属于当前控制者。

## 从 V2.0 升级

V2 的 NVS namespace 为 `cbot`，V3 为 `combatfusion`，**不会自动读取或覆盖旧配置**。先导出 V2 参数并保留旧固件；协议、字段和部分单位已变，旧 PID、标定及自定义客户端不能直接照搬。

旧 6 路 IR 为前/后/左前/右前/左后/右后；V3 为前/右前/右后/后/左后/左前。**须按新接线表重接或逐路核对，不能仅升固件。**

可在 `combatbot/` 离线生成迁移候选：

```powershell
python scripts/convert_workbuddy_config.py "V2参数.json" "V3候选.json"
```

工具只生成候选和说明，不连接设备。先以 V3 默认配置为基础，再停车导入并检查；PID 保留 V3 默认，重新标定。旧 `safetyEdge` 或 `safetyIR` 开启时，转换器会推导总开关开启，**导入前核对全部传感器**。见 [完整迁移说明](docs/releases/V3.0.md#升级与回退)。

## 源码与复现

| 路径 | 用途 |
|---|---|
| [CombatBotArduino/](CombatBotArduino/) | Arduino IDE 可直接编译的分文件草图 |
| [combatbot/src/](combatbot/src/) / [combatbot/include/](combatbot/include/) | 固件维护源 |
| [combatbot/web/index.html](combatbot/web/index.html) | 可读可改的完整离线网页 |
| [combatbot/scripts/](combatbot/scripts/) | 导出、验证、迁移、打包工具 |
| [docs/README.md](docs/README.md) | V3 资料与 V2 历史归档索引 |

修改维护源或网页后，在 `combatbot/` 执行以下命令同步导出并重新验证；不要只改导出副本后继续打包。

```powershell
python scripts/export_arduino.py
python scripts/verify_release.py --pio "已有platformio.exe路径" --compiler "已有g++.exe路径" --node "已有node.exe路径"
python scripts/package_delivery.py
python scripts/verify_delivery.py
```

需已有工具链、开发板包及指定依赖，不执行上传。凭据绑定源码、测试和完整日志的 SHA256，改变后须重新验证。最终编译与回归数据见 [验收与验证](验收与验证.md)；主机替身不能替代真实手机、多核时延、掉电恢复及机械验收。

更新记录：[CHANGELOG.md](CHANGELOG.md) · 接口：[docs/接口协议.md](docs/接口协议.md) · 注释：[注释风格规范.md](注释风格规范.md)
