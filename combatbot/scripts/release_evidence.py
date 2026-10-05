"""融合版验证凭据：把成功检查绑定到当前源码与测试，拒绝沿用旧日志。"""
from pathlib import Path
import hashlib
import json

ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "test-artifacts" / "release-evidence.json"


def source_manifest():
    """只收集维护源码、网页、验证脚本和测试；排除缓存及主机二进制。"""
    files = [ROOT / "platformio.ini"]
    for name in ("src", "include", "web", "scripts", "test"):
        files.extend(p for p in (ROOT / name).rglob("*") if p.is_file()
                     and "__pycache__" not in p.parts
                     and p.suffix not in (".exe", ".pyc"))
    return {p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(files)}


def verify_evidence():
    """检查源码未变、所有检查成功且日志内容未被替换；失败时禁止打包。"""
    data = json.loads(EVIDENCE.read_text(encoding="utf-8"))
    if data.get("sources") != source_manifest():
        raise RuntimeError("源码或测试已改变，请重新执行 verify_release.py 后再打包")
    required = {"host", "motor", "ui", "ui-offline", "migration", "core2", "core3",
                "test_session", "test_sensor_model", "core3-native"}
    checks = data.get("checks", {})
    if not required.issubset(checks):
        raise RuntimeError("交付验证缺少必要检查")
    for name, item in checks.items():
        path = ROOT / item["log"]
        if item["exitCode"] != 0 or hashlib.sha256(path.read_bytes()).hexdigest() != item["sha256"]:
            raise RuntimeError(f"验证凭据失效：{name}")
    return data
