"""
CombatBot Fusion · 软件交付验证入口
============================================================================
顺序：导出 → 控制/配置/电机/网页回归 → 实际 Arduino 草图双版本编译。
约定：任一步失败即停止；最终凭据绑定当前源码哈希与每一步完整日志。
边界：只复用已有工具链和依赖，不烧录、不启动串口、不改用户全局配置。
============================================================================
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from release_evidence import ROOT, EVIDENCE, source_manifest


def main():
    parser = argparse.ArgumentParser(description="执行融合工程的全部软件交付检查。")
    parser.add_argument("--pio", default=shutil.which("platformio"))
    parser.add_argument("--compiler", default=shutil.which("g++"))
    parser.add_argument("--node", default=shutil.which("node"))
    parser.add_argument("--native-jobs", type=int, default=min(8, os.cpu_count() or 4))
    parser.add_argument("--native-incremental", action="store_true", help="复用已有原生构建的框架缓存，仍重新编译改动模块")
    args = parser.parse_args()
    if not 1 <= args.native_jobs <= 32:
        parser.error("--native-jobs 必须为1~32")
    for name in ("pio", "compiler", "node"):
        if not getattr(args, name):
            parser.error(f"未找到 {name}，请显式指定其已安装路径")
    subprocess.run([sys.executable, str(ROOT / "scripts/export_arduino.py")], check=True)
    artifacts = ROOT / "test-artifacts"
    artifacts.mkdir(exist_ok=True)
    before = source_manifest()
    checks = {}

    def run(name, command, log_name=None):
        path = artifacts / (log_name or (name + ".log"))
        print(f"正在验证 {name}；日志：{path}", flush=True)
        with path.open("w", encoding="utf-8") as log:
            log.write("Command: " + subprocess.list2cmdline(command) + "\n")
            log.flush()
            result = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
            log.write(f"\n[exit code: {result.returncode}]\n")
        checks[name] = {"exitCode": result.returncode,
                        "log": path.relative_to(ROOT).as_posix(),
                        "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
        if result.returncode:
            print(path.read_text(encoding="utf-8", errors="replace")[-12000:], flush=True)
            raise RuntimeError(f"{name} 失败，退出码 {result.returncode}")
        print(f"{name}: PASS", flush=True)

    ps = shutil.which("powershell") or "powershell"
    run("host", [ps, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                 str(ROOT / "test/run_host_tests.ps1"), "-Compiler", args.compiler])
    run("motor", [ps, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                  str(ROOT / "test/run_motor_tests.ps1"), "-Compiler", args.compiler])
    # 新增的纯会话/传感器模型测试由各模块交付；使用真实模块和已有平台替身。
    for name in ("test_session", "test_sensor_model"):
        source = ROOT / "test" / (name + ".cpp")
        if not source.exists():
            raise RuntimeError(f"缺少必要新增测试：{source}")
        executable = artifacts / (name + ".exe")
        includes = ["-Iinclude"]
        sources = [str(source)]
        if name == "test_sensor_model":
            includes.insert(0, "-Itest/sensor_host")
            sources += ["src/sensors.cpp", "src/imu.cpp"]
        else:
            includes.insert(0, "-Itest/host")
        run(name + "-compile", [args.compiler, "-std=c++17", "-static", "-Wall", "-Wextra", "-Werror"]
            + includes + sources + ["-o", str(executable)])
        run(name, [str(executable)])
    run("ui", [args.node, "test/ui_contract.mjs"])
    run("ui-offline", [args.node, "test/ui_contract.mjs", "--file"])
    run("migration", [sys.executable, "test/test_migration.py"])
    run("core2", [sys.executable, "scripts/verify_arduino.py", "--pio", args.pio], "core2-compatibility.log")
    native_command=[sys.executable, "scripts/verify_arduino_cli.py", "--jobs", str(args.native_jobs)]
    if args.native_incremental:
        native_command.append("--incremental")
    run("core3", native_command)
    native_log = artifacts / "ide-core3-default.log"
    if not native_log.read_text(encoding="utf-8").rstrip().endswith("[exit code: 0]"):
        raise RuntimeError("原生编译完整日志缺少成功退出记录")
    checks["core3-native"] = {"exitCode": 0, "log": native_log.relative_to(ROOT).as_posix(),
                              "sha256": hashlib.sha256(native_log.read_bytes()).hexdigest()}
    # 原生编译器自身还生成包含 FQBN/工具版本的日志，打包时使用它。
    if source_manifest() != before:
        raise RuntimeError("验证期间源码改变，拒绝发布凭据；等待编辑完成后重跑")
    EVIDENCE.write_text(json.dumps({"verifiedAtUtc": datetime.now(timezone.utc).isoformat(),
                                    "sources": before, "checks": checks},
                                   indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"全部软件验证通过；凭据：{EVIDENCE}", flush=True)
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"验证中止：{error}", file=sys.stderr)
        raise SystemExit(1)
