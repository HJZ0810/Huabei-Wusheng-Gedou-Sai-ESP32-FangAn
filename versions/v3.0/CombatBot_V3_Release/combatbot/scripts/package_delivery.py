"""
CombatBot · 可分发工程包生成
============================================================================
内容：Arduino 草图、指定版本依赖库、维护源码、测试与使用说明。
校验：为每个交付模块生成 SHA256；完整压缩包由 verify_delivery.py 复核。
边界：不收录本机工具链、缓存与主机测试二进制，避免交付包绑定当前机器。
============================================================================
"""
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED
import json, hashlib
from release_evidence import verify_evidence
from verify_arduino_cli import verify_export

# 编译日志不能脱离当前源码使用；先核对完整验证凭据，再写入任何交付产物。
evidence = verify_evidence()
verify_export()

ROOT = Path(__file__).resolve().parents[1]
DELIVERY = ROOT.parent
libraries = DELIVERY / "Arduino依赖库"
libraries.mkdir(exist_ok=True)
versions = {"ArduinoJson": "7.3.1", "AsyncTCP": "3.3.2", "ESPAsyncWebServer": "3.6.0"}
# 依赖库保留原目录与许可证，便于通过 Arduino IDE 的 ZIP 库安装入口导入。
for name, version in versions.items():
    source = ROOT / ".pio" / "libdeps" / "esp32s3" / name
    if not source.is_dir():
        raise FileNotFoundError(source)
    properties = dict(line.split("=", 1) for line in
                      (source / "library.properties").read_text(encoding="utf-8").splitlines()
                      if "=" in line and not line.startswith("#"))
    if properties.get("version") != version:
        raise ValueError(f"Library version mismatch: {name}: {properties.get('version')} != {version}")
    with ZipFile(libraries / f"{name}-{version}.zip", "w", ZIP_DEFLATED) as output:
        for path in sorted(source.rglob("*")):
            if path.is_file() and ".git" not in path.parts and path.name != ".piopm":
                output.write(path, Path(name) / path.relative_to(source))
manifest = {}
# 清单使用正斜线，与 ZIP 内的路径格式一致，方便跨平台校验。
sketch = DELIVERY / "CombatBotArduino"
for path in sorted(sketch.iterdir()):
    if path.is_file():
        manifest[path.relative_to(DELIVERY).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
(DELIVERY / "源码SHA256.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False)+"\n", encoding="utf-8")
archive = DELIVERY / "CombatBot_V3.0_Arduino完整工程.zip"
build_logs = {
    "ArduinoESP32-2.0.17.log": ROOT / "test-artifacts" / "core2-compatibility.log",
    "ArduinoESP32-3.3.12.log": ROOT / "test-artifacts" / "ide-core3-default.log",
}
# 双版本验证未完成时拒绝生成最终包，避免把仍在构建中的目录提前交付。
if "[SUCCESS]" not in build_logs["ArduinoESP32-2.0.17.log"].read_text(encoding="utf-8-sig"):
    raise RuntimeError("Arduino ESP32 2.0.17 build has not passed")
if not build_logs["ArduinoESP32-3.3.12.log"].read_text(encoding="utf-8").rstrip().endswith("[exit code: 0]"):
    raise RuntimeError("Native Arduino ESP32 3.3.12 build has not passed")
with ZipFile(archive,"w",ZIP_DEFLATED) as output:
    for base in (sketch,libraries):
        for path in sorted(base.rglob("*")):
            if path.is_file(): output.write(path,path.relative_to(DELIVERY))
    for name in ("README.md","验收与验证.md","注释风格规范.md","源码SHA256.json","安装Arduino依赖.ps1",
                 "融合说明与参数表.md","新手安装与接线.md","CHANGELOG.md","VERSION","LICENSE"):
        output.write(DELIVERY/name,name)
    for path in sorted((DELIVERY / "docs").rglob("*")):
        if path.is_file(): output.write(path,path.relative_to(DELIVERY))
    output.write(ROOT/"web"/"index.html","网页源码/index.html")
    for name,path in build_logs.items():
        output.write(path,Path("验证日志")/name)
    output.write(ROOT/"test-artifacts"/"release-evidence.json","combatbot/test-artifacts/release-evidence.json")
    for item in evidence["checks"].values():
        path = ROOT / item["log"]
        output.write(path,Path("combatbot")/item["log"])
    # 维护源码只从白名单目录收集，不递归遍历 .tools/.pio 等构建目录。
    for sub in ("src","include","scripts","test"):
        for path in sorted((ROOT/sub).rglob("*")):
            if path.is_file() and path.suffix not in (".exe",".pyc") and "__pycache__" not in path.parts:
                output.write(path,Path("combatbot")/path.relative_to(ROOT))
    output.write(ROOT/"platformio.ini","combatbot/platformio.ini")
print(f"Packaged {archive.name}: {archive.stat().st_size} bytes")
