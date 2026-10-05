# 历代工程与原始资料

这里保留能够独立查看的历史工程。原始源码、README、许可证和文档均按对应 Git 提交逐字节恢复，扩写说明另行存放，避免把后来的修复写进旧版本。

| 版本 | 工程快照 | 原始使用说明 | 扩写版本说明 | Git 基线 |
|---|---|---|---|---|
| V1.0 | [CombatBot_ESP32S3](v1.0/CombatBot_ESP32S3/) | [原始 README](v1.0/CombatBot_ESP32S3/README.md) | [V1.0 功能与边界](../docs/releases/V1.0.md) | [1b04fd7](https://github.com/HJZ0810/Huabei-Wusheng-Gedou-Sai-ESP32-FangAn/commit/1b04fd7485a531d5d85fe769732636cfa675b90e) |
| V2.0 | [CombatBot_Fusion](v2.0/CombatBot_Fusion/) | [原始 README](v2.0/CombatBot_Fusion/README.md) | [V2.0 加固与演进](../docs/releases/V2.0.md) | [fd650ac](https://github.com/HJZ0810/Huabei-Wusheng-Gedou-Sai-ESP32-FangAn/commit/fd650ac14d98aebc9779bf88365e23ad4cc5305f) |
| V3.0 | [当前 Arduino 工程](../CombatBotArduino/) | [当前项目 README](../README.md) | [V3.0 差异与迁移](../docs/releases/V3.0.md) | [固定固件标签 v3.0](https://github.com/HJZ0810/Huabei-Wusheng-Gedou-Sai-ESP32-FangAn/tree/v3.0) |

## 如何阅读和使用

V1.0 建立完整的运动、感知、标定、网络与网页功能；V2.0 针对急停、并发控制、输入和反馈做加固；V3.0 进一步修正控制组合、保存失败和会话边界，并绑定实际验证记录。详细记录在根 README 中全部展开，新版本持续向顶部追加。

旧工程的主 `.ino` 与所在草图目录同名，可分别打开对应目录。不要把不同版本的 `.ino/.cpp/.h` 合并到一个草图中；各版本依赖、引脚含义、参数单位、NVS 命名空间和协议应按各自说明核对。

原始说明完整保留，其中的编译、精度、时延和硬件表述属于当时记录。本次只恢复历史内容并补充说明，未重新编译或上板验证 V1/V2。旧版本的已知问题不会因为归档而获得修复，使用前先阅读对应扩写说明。

## 完整性与后续维护

[SOURCE_MANIFEST.json](SOURCE_MANIFEST.json) 记录每份快照的来源提交、目录和全部文件 SHA256。本次 V1.0 为 31 个文件，V2.0 为 37 个文件，原始 README、源代码和文档均在其中。

历史快照只读。发现历史说明错误时，在扩写说明或更新记录中补充勘误并注明来源，不直接改写快照。发布新版本时保留现有快照与记录，新增该版说明及入口；归档新工程时避免把已有 `versions/` 再次递归复制进去。

完整包通过 [package_history.py](../tools/package_history.py) 生成：先执行当前软件交付门控，再将校验通过的旧工程、清单和维护工具纳入包中。原有 [V2 文档归档](../docs/archive/v2.0/) 继续保留。
