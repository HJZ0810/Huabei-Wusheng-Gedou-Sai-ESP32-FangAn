# 文档索引

## V5.0 开发预备资料

V5预备分支已联通融合工作台、显式横屏、640×640未知态雷达、全量离线默认草稿、当前阶段完整剩余路线及点选即前往。开发人工调试默认关闭，启用后最终PWM≤180/1023、单次最长15秒；标定包含IMU、100cm前进行程、左右转独立修正与零PWM登台观测，应用绑定结果、会话与配置代次。

两份UI合同和file离线初始化已通过。19条软件门控全部通过，Arduino-ESP32 2.0.17与3.3.12 Huge APP实际编译链接成功；82项默认配置、离线交互、标定、路线及手机横屏视觉均完成检查。当前V5仅本地交付，GitHub与公网仍使用V4。没有新增舵机限位自标定、直行偏差自动修正或登台参数自动建议；公网配置与标定仍连接本地车辆Wi-Fi。未上板，V4的成功凭据不作为V5验证。

- [V5工作台与调试设计](design/V5融合工作台与调试设计.md)：当前布局、旋转输入、草稿、调试限幅、路线和标定绑定。
- [V5预备记录](releases/V5.0.md)：V4→V5实际进步、尚未实现能力及验证/发布状态。

## V4.0 已发布资料

V4使用分文件Arduino工程，AP本地与STA公网共用网页；估计地图和有界自主在车端运行。公网入口为 [combatbot.luo-jin-ai.com](https://combatbot.luo-jin-ai.com/)，专属控制链接无需账户或选车，操作前手动接管。独立服务、公网协议替身与专属链接浏览器检查已通过；最终固件双编译与全量软件门控已通过，完整包及GitHub发布均已完成；证据见发布记录。旧版本源码使用固定标签与历史目录，不能把当前维护目录当作V3快照。

| 资料 | 内容 |
|---|---|
| [项目入口](../README.md) | 下载、编译、运行和保护边界 |
| [V4发布记录](releases/V4.0.md) | V3 → V4 差异、进步、取舍与验证证据 |
| [V4安装与校准](hardware/V4传感器安装与校准.md) | 原创安装图、探头坐标、电平与支撑检测 |
| [V4服务器部署](setup/V4服务器部署.md) | 单车域名/证书/进程、公网协议替身检查与回退 |
| [V4联网与比赛操作](setup/V4联网与比赛操作.md) | STA/CA配置、专属链接、地图任务与接管 |
| [V4自主控制与定位](design/V4自主控制与定位.md) | 分层地图、估计误差、导航、登台、格斗与断网继续 |
| [V3发布与迁移历史](releases/V3.0.md) | V2 → V3 差异、进步、取舍与回退 |
| [更新记录](../CHANGELOG.md) | 按版本归纳变化 |
| [新手安装与接线](../新手安装与接线.md) | 当前依赖、开发板设置、引脚与排错 |
| [参数表](../融合说明与参数表.md) | 字段、单位、策略与离线转换 |
| [接口协议](接口协议.md) | 当前 HTTP/WebSocket 接口及令牌 |
| [验收与验证](../验收与验证.md) | 编译、回归、发布凭据及装机项目 |
| [注释规范](../注释风格规范.md) | 中文 Doxygen、单位与设计边界 |

推荐阅读顺序：[安装与接线](../新手安装与接线.md) → [传感器校准](hardware/V4传感器安装与校准.md) → [联网与比赛操作](setup/V4联网与比赛操作.md)。维护者再看[设计](design/V4自主控制与定位.md)、[协议](接口协议.md)与[验证](../验收与验证.md)。软件通过不等于机械爬阶或实车防掉通过。

Arduino 入口：[CombatBotArduino.ino](../CombatBotArduino/CombatBotArduino.ino)。维护源码：[combatbot](../combatbot/)。

## 全部版本与演进资料

根 [README 的完整版本更新记录](../README.md#完整版本更新记录) 全部展开 V5（本地预备交付）、V4、V3、V2、V1。新增版本从顶部追加，旧版本的完整源码、原始说明、扩写资料和发布入口持续保留。

| 版本 | 扩写说明 | 完整旧工程 / 当前工程 | 原始说明 | 发布入口 |
|---|---|---|---|---|
| V5.0 | [开发预备记录](releases/V5.0.md) | [当前维护分支](../combatbot/)（本地验证通过） | [V5 设计](design/V5融合工作台与调试设计.md) | 尚未发布 |
| V4.0 | [云端与自主扩展](releases/V4.0.md) | [固定 V4 Arduino 草图](https://github.com/HJZ0810/Huabei-Wusheng-Gedou-Sai-ESP32-FangAn/tree/v4.0/CombatBotArduino) | [V4 发布 README](https://github.com/HJZ0810/Huabei-Wusheng-Gedou-Sai-ESP32-FangAn/blob/v4.0/README.md) | [V4.0 Release](https://github.com/HJZ0810/Huabei-Wusheng-Gedou-Sai-ESP32-FangAn/releases/tag/v4.0) |
| V3.0 | [控制正确性与可验证交付](releases/V3.0.md) | [固定V3 Arduino源码](https://github.com/HJZ0810/Huabei-Wusheng-Gedou-Sai-ESP32-FangAn/tree/v3.0/CombatBotArduino) | [固定发布提交](https://github.com/HJZ0810/Huabei-Wusheng-Gedou-Sai-ESP32-FangAn/tree/v3.0) | [V3.0](https://github.com/HJZ0810/Huabei-Wusheng-Gedou-Sai-ESP32-FangAn/releases/tag/v3.0) |
| V2.0 | [并发、安全与异常处理加固](releases/V2.0.md) | [CombatBot_Fusion](../versions/v2.0/CombatBot_Fusion/) | [V2 原始 README](../versions/v2.0/CombatBot_Fusion/README.md) | [V2.0 历史归档](https://github.com/HJZ0810/Huabei-Wusheng-Gedou-Sai-ESP32-FangAn/releases/tag/v2.0) |
| V1.0 | [完整控制与标定链路](releases/V1.0.md) | [CombatBot_ESP32S3](../versions/v1.0/CombatBot_ESP32S3/) | [V1 原始 README](../versions/v1.0/CombatBot_ESP32S3/README.md) | [V1.0 历史归档](https://github.com/HJZ0810/Huabei-Wusheng-Gedou-Sai-ESP32-FangAn/releases/tag/v1.0) |

来源提交与逐文件校验见 [历代工程索引](../versions/README.md) 和 [历史清单](../versions/SOURCE_MANIFEST.json)。完整包使用 [历史资料打包入口](../tools/package_history.py)，同时保留当前软件验证门控和旧工程完整性检查。

## V2 历史归档

以下资料保留作历史对照，其中草图、依赖、PWM 位数、协议、默认保护及参数可能属于 V2，**请勿据此安装或配置 V4**。当前接线和运行说明以上方 V4 资料为准。

- [V2 Arduino 环境指南](archive/v2.0/setup/新手文档_Arduino环境搭建.md)
- [V2 接线表](archive/v2.0/hardware/接线表.md)
- [V2 自检清单](archive/v2.0/verification/自检清单.md)
- [V2 详细设计](archive/v2.0/design/详细设计方案_ESP32S3格斗车.md)
- [模型核实与参数历史](archive/v2.0/design/模型核实与实测参数.md)
- [历史需求提示词](archive/v2.0/archive/提示词_ESP32S3格斗车.md)

历史代码以 [V2 基线提交](https://github.com/HJZ0810/Huabei-Wusheng-Gedou-Sai-ESP32-FangAn/commit/fd650ac14d98aebc9779bf88365e23ad4cc5305f) 为准。作者及许可证见 [LICENSE](../LICENSE)。原始 STEP、零件 ZIP 及全套模型截图不在本仓库。
