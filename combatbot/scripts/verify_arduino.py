"""
CombatBot · Arduino 交付目录编译验证
============================================================================
职责：先检查模块导出一致性，再使用 Arduino 编译链编译实际交付的 .ino。
边界：验证源码组织与链接，不执行上传，也不证明真实设备的运动性能。
约定：仅入口 @file 名称随导出改变；生成的头文件和业务模块须保持一致。
============================================================================
"""
from pathlib import Path
import argparse, configparser, shutil, subprocess
from release_evidence import dependency_manifest
from verify_arduino_cli import verify_export

parser = argparse.ArgumentParser(description="编译导出的 Arduino 草图，不上传固件。")
parser.add_argument("--pio", help="已有 PlatformIO 可执行程序路径；未指定时从 PATH 查找")
args = parser.parse_args()

ROOT = Path(__file__).resolve().parents[1]
SKETCH = ROOT.parent / "CombatBotArduino"
verify_export()
dependency_manifest()
for path in (ROOT / "src").glob("*.cpp"):
    delivered = SKETCH / ("CombatBotArduino.ino" if path.name == "main.cpp" else path.name)
    expected = path.read_bytes()
    if path.name == "main.cpp":
        expected = path.read_text(encoding="utf-8").replace(
            "@file    main.cpp", "@file    CombatBotArduino.ino", 1
        ).encode("utf-8")
    assert delivered.read_bytes() == expected, f"Export outdated: {delivered}"
for path in (ROOT / "include").glob("*.h"):
    assert (SKETCH / path.name).read_bytes() == path.read_bytes(), f"Export outdated: {path.name}"

config = configparser.ConfigParser()
# 复用依赖版本与开发板选项，只替换源码位置与构建输出路径。
config.read(ROOT / "platformio.ini", encoding="utf-8")
config["platformio"]["src_dir"] = SKETCH.as_posix()
config["platformio"]["include_dir"] = SKETCH.as_posix()
config["platformio"]["libdeps_dir"] = (ROOT / ".pio" / "libdeps").as_posix()
config["platformio"]["build_dir"] = (ROOT / ".pio" / "build-arduino").as_posix()
# 网页头已在导出阶段生成，此处不重复运行维护目录的构建前脚本。
config["env:esp32s3"].pop("extra_scripts", None)
project = ROOT / "test-artifacts" / "arduino-layout"
project.mkdir(parents=True,exist_ok=True)
with (project / "platformio.ini").open("w",encoding="utf-8") as output:
    config.write(output)
pio = ROOT / ".tools" / "venv" / "Scripts" / "platformio.exe"
command = args.pio or (str(pio) if pio.exists() else shutil.which("platformio"))
if not command: raise RuntimeError("Install PlatformIO before running this verification")
print("Export equality OK; building delivered CombatBotArduino.ino", flush=True)
try:
    result = subprocess.call([command,"run","-d",str(project),"-e","esp32s3","-j","4"])
finally:
    # PIO会在草图旁生成同名 .ino.cpp；仅清理此已知产物，不能让它参与下一次IDE编译。
    generated = (SKETCH / "CombatBotArduino.ino.cpp").resolve()
    if generated.parent != SKETCH.resolve():
        raise RuntimeError("PIO预处理产物路径越界")
    generated.unlink(missing_ok=True)
raise SystemExit(result)
