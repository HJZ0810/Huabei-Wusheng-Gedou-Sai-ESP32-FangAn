# 文档索引

## V3.0 当前资料

| 资料 | 内容 |
|---|---|
| [项目入口](../README.md) | 下载、编译、运行和保护边界 |
| [发布与迁移](releases/V3.0.md) | V2 → V3 差异、进步、取舍与回退 |
| [更新记录](../CHANGELOG.md) | 按版本归纳变化 |
| [新手安装与接线](../新手安装与接线.md) | 当前依赖、开发板设置、引脚与排错 |
| [参数表](../融合说明与参数表.md) | 字段、单位、策略与离线转换 |
| [接口协议](接口协议.md) | 当前 HTTP/WebSocket 接口及令牌 |
| [验收与验证](../验收与验证.md) | 编译、回归、发布凭据及装机项目 |
| [注释规范](../注释风格规范.md) | 中文 Doxygen、单位与设计边界 |

Arduino 入口：[CombatBotArduino.ino](../CombatBotArduino/CombatBotArduino.ino)。维护源码：[combatbot](../combatbot/)。

## 全部版本与演进资料

根 [README 的完整版本更新记录](../README.md#完整版本更新记录) 全部展开 V3、V2、V1。新增版本从顶部追加，旧版本的完整源码、原始说明、扩写资料和发布入口持续保留。

| 版本 | 扩写说明 | 完整旧工程 / 当前工程 | 原始说明 | 发布入口 |
|---|---|---|---|---|
| V3.0 | [控制正确性与可验证交付](releases/V3.0.md) | [当前 Arduino 草图](../CombatBotArduino/) | [固定发布提交](https://github.com/HJZ0810/Huabei-Wusheng-Gedou-Sai-ESP32-FangAn/tree/v3.0) | [V3.0](https://github.com/HJZ0810/Huabei-Wusheng-Gedou-Sai-ESP32-FangAn/releases/tag/v3.0) |
| V2.0 | [并发、安全与异常处理加固](releases/V2.0.md) | [CombatBot_Fusion](../versions/v2.0/CombatBot_Fusion/) | [V2 原始 README](../versions/v2.0/CombatBot_Fusion/README.md) | [V2.0 历史归档](https://github.com/HJZ0810/Huabei-Wusheng-Gedou-Sai-ESP32-FangAn/releases/tag/v2.0) |
| V1.0 | [完整控制与标定链路](releases/V1.0.md) | [CombatBot_ESP32S3](../versions/v1.0/CombatBot_ESP32S3/) | [V1 原始 README](../versions/v1.0/CombatBot_ESP32S3/README.md) | [V1.0 历史归档](https://github.com/HJZ0810/Huabei-Wusheng-Gedou-Sai-ESP32-FangAn/releases/tag/v1.0) |

来源提交与逐文件校验见 [历代工程索引](../versions/README.md) 和 [历史清单](../versions/SOURCE_MANIFEST.json)。完整包使用 [历史资料打包入口](../tools/package_history.py)，同时保留当前软件验证门控和旧工程完整性检查。

## V2 历史归档

以下资料保留作历史对照，其中草图、依赖、PWM 位数、协议、默认保护及参数可能属于 V2，**请勿据此安装或配置 V3**。当前接线和运行说明以上方 V3 资料为准。

- [V2 Arduino 环境指南](archive/v2.0/setup/新手文档_Arduino环境搭建.md)
- [V2 接线表](archive/v2.0/hardware/接线表.md)
- [V2 自检清单](archive/v2.0/verification/自检清单.md)
- [V2 详细设计](archive/v2.0/design/详细设计方案_ESP32S3格斗车.md)
- [模型核实与参数历史](archive/v2.0/design/模型核实与实测参数.md)
- [历史需求提示词](archive/v2.0/archive/提示词_ESP32S3格斗车.md)

历史代码以 [V2 基线提交](https://github.com/HJZ0810/Huabei-Wusheng-Gedou-Sai-ESP32-FangAn/commit/fd650ac14d98aebc9779bf88365e23ad4cc5305f) 为准。作者及许可证见 [LICENSE](../LICENSE)。原始 STEP、零件 ZIP 及全套模型截图不在本仓库。
