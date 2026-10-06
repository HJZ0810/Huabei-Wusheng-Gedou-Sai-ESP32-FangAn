"""
CombatBot · 可分发工程包生成
============================================================================
内容：Arduino 草图、指定版本依赖库、维护源码、测试与使用说明。
校验：为每个交付模块生成 SHA256；完整压缩包由 verify_delivery.py 复核。
边界：不收录本机工具链、缓存与主机测试二进制，避免交付包绑定当前机器。
============================================================================
"""
from pathlib import Path
from zipfile import ZipFile, ZipInfo, ZIP_DEFLATED
import json
from release_evidence import (ROOT, DELIVERY, archive_name, sha, verify_evidence,
                              dependency_specs, verify_library_archive)
from verify_arduino_cli import verify_export

# 先核对全部源码、补丁依赖和日志；旧V3或单次预编译不能通过此门控。
evidence = verify_evidence()
verify_export()
files = {}
for path in sorted((DELIVERY / "CombatBotArduino").iterdir()):
    files[path.relative_to(DELIVERY).as_posix()] = path
for name in evidence["sources"]:
    files[name] = DELIVERY / name
for spec in dependency_specs():
    target = DELIVERY / "Arduino依赖库" / f"{spec['name']}-{spec['version']}.zip"
    verify_library_archive(target, spec, evidence["dependencies"][spec["name"]])
for name in ("README.md", "验收与验证.md", "注释风格规范.md", "安装Arduino依赖.ps1",
             "融合说明与参数表.md", "新手安装与接线.md", "CHANGELOG.md", "VERSION", "LICENSE",
             "依赖版本.json", "server/README.md"):
    files[name] = DELIVERY / name
for path in sorted((DELIVERY / "docs").rglob("*")):
    if path.is_file(): files[path.relative_to(DELIVERY).as_posix()] = path
files["网页源码/index.html"] = ROOT / "web/index.html"
files["combatbot/test-artifacts/release-evidence.json"] = ROOT / "test-artifacts/release-evidence.json"
for item in evidence["checks"].values():
    files["combatbot/" + item["log"]] = ROOT / item["log"]
core2 = ROOT / evidence["checks"]["core2"]["log"]
native = ROOT / evidence["checks"]["core3-native"]["log"]
if "[SUCCESS]" not in core2.read_text(encoding="utf-8-sig"):
    raise RuntimeError("core2日志缺少成功标记")
if not native.read_text(encoding="utf-8").rstrip().endswith("[exit code: 0]"):
    raise RuntimeError("core3原生日志缺少成功退出标记")
fqbn = evidence["builds"]["core3"]["fqbn"]
if "FQBN: " + fqbn + "\n" not in native.read_text(encoding="utf-8"):
    raise RuntimeError("core3实际FQBN与发布凭据不一致")
files["验证日志/ArduinoESP32-2.0.17.log"] = DELIVERY / "验证日志" / "ArduinoESP32-2.0.17.log"
files["验证日志/ArduinoESP32-3.3.12-" + evidence["builds"]["core3"]["buildName"] + ".log"] = (
    DELIVERY / "验证日志" / f"ArduinoESP32-3.3.12-{evidence['builds']['core3']['buildName']}.log"
)
for name, path in files.items():
    if not path.resolve().is_relative_to(DELIVERY.resolve()):
        raise RuntimeError(f"交付路径越界：{name}")
manifest = {name: sha(path.read_bytes()) for name, path in sorted(files.items())}
manifest_path = DELIVERY / "源码SHA256.json"
manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
files["源码SHA256.json"] = manifest_path
archive = DELIVERY / archive_name()
with ZipFile(archive, "w", ZIP_DEFLATED) as output:
    for name, path in sorted(files.items()):
        info = ZipInfo(name, (1980, 1, 1, 0, 0, 0))
        info.compress_type = ZIP_DEFLATED
        output.writestr(info, path.read_bytes())
print(f"Packaged {archive.name}: {archive.stat().st_size} bytes; {len(files)} checked entries")
