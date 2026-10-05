"""
CombatBot · Arduino IDE 原生编译验证
============================================================================
职责：校验交付源码与维护源码一致，并使用 IDE 内置 Arduino CLI 编译实际草图。
边界：复用已安装的开发板与用户库；不安装依赖、不修改全局设置、不上传固件。
产物：独立配置、编译日志和构建目录统一写入 test-artifacts，便于复核与定位。
============================================================================
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from release_evidence import create_dependency_archives, prepare_isolated_libraries, dependency_specs


ROOT = Path(__file__).resolve().parents[1]
SKETCH = ROOT.parent / "CombatBotArduino"
ESP32_INDEX = "https://espressif.github.io/arduino-esp32/package_esp32_index.json"


def verify_export() -> None:
    """逐文件校验导出内容；仅允许草图入口的 @file 名称随文件名改变。"""
    expected_names = set()
    for folder, pattern in (("src", "*.cpp"), ("include", "*.h")):
        for source in (ROOT / folder).glob(pattern):
            expected = source.read_bytes()
            name = source.name
            if name == "main.cpp":
                name = "CombatBotArduino.ino"
                expected = source.read_text(encoding="utf-8").replace(
                    "@file    main.cpp", "@file    CombatBotArduino.ino", 1
                ).encode("utf-8")
            target = SKETCH / name
            expected_names.add(name)
            if not target.is_file() or target.read_bytes() != expected:
                raise RuntimeError(f"交付源码未同步：{target}，请先运行 export_arduino.py")
    actual_names = {path.name for path in SKETCH.iterdir() if path.is_file()}
    if actual_names != expected_names:
        raise RuntimeError("草图文件集合不匹配，拒绝多余模块/PIO生成文件：" +
                           str(sorted(actual_names.symmetric_difference(expected_names))))


def find_cli(explicit: str | None) -> Path:
    """优先使用显式路径，其次查找常见 IDE 安装位置，最后查询 PATH。"""
    if explicit:
        candidate = Path(explicit).expanduser().resolve()
        if not candidate.is_file():
            raise RuntimeError(f"Arduino CLI 不存在：{candidate}")
        return candidate
    relative = Path("Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe")
    candidates = [
        Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData/Local"))
        / "Programs" / relative,
        Path(os.environ.get("ProgramFiles", "C:/Program Files")) / relative,
        Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / relative,
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    executable = shutil.which("arduino-cli")
    if executable:
        return Path(executable).resolve()
    raise RuntimeError("未找到 Arduino CLI；请安装 Arduino IDE 或通过 --cli 指定已有程序。")


def run_logged(command: list[str], log) -> subprocess.CompletedProcess[str]:
    """保存完整输出与退出码；日志显式使用 UTF-8，避免中文诊断被系统编码损坏。"""
    log.write("\n> " + subprocess.list2cmdline(command) + "\n")
    log.flush()
    process = subprocess.Popen(
        command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, encoding="utf-8", errors="replace",
    )
    lines = []
    assert process.stdout is not None
    for line in process.stdout:
        lines.append(line)
        log.write(line)
        log.flush()
    result = subprocess.CompletedProcess(command, process.wait(), "".join(lines))
    log.write(f"\n[exit code: {result.returncode}]\n")
    log.flush()
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description="使用 Arduino IDE 原生 CLI 编译交付草图，不上传固件。")
    parser.add_argument("--fqbn", default="esp32:esp32:esp32s3", help="完整开发板标识及可选菜单参数")
    parser.add_argument("--build-name", help="产物前缀；未指定时按实际FQBN选择default或huge-app")
    parser.add_argument("--cli", help="已有 arduino-cli 可执行文件的完整路径")
    parser.add_argument("--jobs", type=int, default=4, help="编译并发数，范围1~32；默认4")
    parser.add_argument("--incremental", action="store_true", help="保留当前构建缓存，由CLI按改动重新编译；默认clean")
    parser.add_argument("--user-dir", type=Path, help="本工程test-artifacts中的隔离用户库目录；默认按四库内容哈希新建")
    args = parser.parse_args()
    if not 1 <= args.jobs <= 32:
        parser.error("--jobs 必须为1~32")
    if not args.fqbn.startswith("esp32:esp32:esp32s3"):
        parser.error("本交付只验证 esp32:esp32:esp32s3 及其菜单参数")
    if not args.build_name:
        args.build_name = "ide-core3-huge-app" if "PartitionScheme=huge_app" in args.fqbn else "ide-core3-default"
    if "PartitionScheme=huge_app" in args.fqbn and args.build_name == "ide-core3-default":
        parser.error("Huge APP 构建不能使用默认分区日志名称")
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", args.build_name):
        parser.error("--build-name 必须为安全文件名前缀，不允许路径分隔符")

    # 01. 编译前先确认交付内容，避免维护目录通过而用户实际拿到的草图仍旧过期。
    verify_export()
    cli = find_cli(args.cli)
    artifact_dir = ROOT / "test-artifacts"
    artifact_dir.mkdir(parents=True, exist_ok=True)
    data_dir = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData/Local")) / "Arduino15"
    create_dependency_archives()
    user_dir = prepare_isolated_libraries(args.user_dir)
    config_path = artifact_dir / f"{args.build_name}.arduino-cli.yaml"
    log_path = artifact_dir / f"{args.build_name}.log"
    build_path = artifact_dir / f"{args.build_name}-build"

    # 02. JSON 字符串也是合法 YAML 标量，能安全保留 Windows 路径与中文目录。
    #     配置仅替换本次 CLI 的索引设置；不读取或覆盖用户全局配置文件。
    scalar = lambda value: json.dumps(str(value), ensure_ascii=False)
    config_path.write_text(
        "board_manager:\n  additional_urls:\n    - " + scalar(ESP32_INDEX)
        + "\ndirectories:\n  data: " + scalar(data_dir)
        + "\n  downloads: " + scalar(data_dir / "staging")
        + "\n  user: " + scalar(user_dir) + "\n",
        encoding="utf-8",
    )
    base = [str(cli), "--config-file", str(config_path)]
    print(f"交付源码一致；正在编译 {SKETCH}", flush=True)
    print(f"完整日志：{log_path}", flush=True)
    with log_path.open("w", encoding="utf-8") as log:
        log.write(f"Sketch: {SKETCH}\nFQBN: {args.fqbn}\nUser libraries: {user_dir}\n")
        log.write("Dependency versions: " + ", ".join(f"{s['name']}={s['version']}" for s in dependency_specs()) + "\n")
        log.write("WebSockets: audited CombatBot patch; TLS CA required; timeout units fixed\n")
        version = run_logged(base + ["version"], log)
        cores = run_logged(base + ["core", "list"], log)
        print("Arduino CLI：" + version.stdout.strip(), flush=True)
        print("已安装开发板版本：\n" + cores.stdout.strip(), flush=True)
        if version.returncode or cores.returncode:
            print(f"环境检查失败，详情见：{log_path}", file=sys.stderr)
            return 1
        if not re.search(r"^esp32:esp32\s+3\.3\.12\s", cores.stdout, re.MULTILINE):
            raise RuntimeError("本交付需要已安装的 ESP32 core 3.3.12；未修改全局开发板配置")

        # 03. 默认 clean；显式增量模式仍由 CLI 跟踪依赖并重编译改动模块，不触发烧录。
        result = run_logged(base + [
            "compile", "--fqbn", args.fqbn, "--jobs", str(args.jobs),
        ] + ([] if args.incremental else ["--clean"]) + [
            "--build-path", str(build_path), str(SKETCH),
        ], log)
    print(result.stdout[-16000:], flush=True)
    if result.returncode:
        print(f"编译失败（退出码 {result.returncode}），完整日志：{log_path}", file=sys.stderr)
        return result.returncode
    print(f"编译成功；完整日志：{log_path}", flush=True)
    return 0


if __name__ == "__main__":
    # Windows 重定向输出时可能使用本地代码页，统一为 UTF-8 便于归档及工具读取。
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"验证失败：{error}", file=sys.stderr)
        raise SystemExit(1)
