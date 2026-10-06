"""交付包核验：逐条目 SHA256、固定依赖、当前凭据及离线资源。

版本来自 VERSION；软件验证凭据只能来自当前完整门控。历史快照由
tools/package_history.py 追加，并按独立清单验证，不参与当前固件编译。
"""
from pathlib import PurePosixPath
from zipfile import ZipFile
import gzip
import io
import json
import re

from release_evidence import (DELIVERY, ROOT, archive_name, version, sha,
                              dependency_specs, verify_library_archive, verify_evidence)
from verify_arduino_cli import verify_export


def verify_archive(archive):
    evidence = verify_evidence()
    verify_export()
    with ZipFile(archive) as z:
        names = z.namelist()
        if z.testzip() is not None or len(names) != len(set(names)):
            raise RuntimeError("交付 ZIP 损坏或存在重复条目")
        for name in names:
            path = PurePosixPath(name)
            if path.is_absolute() or ".." in path.parts or "\\" in name:
                raise RuntimeError(f"交付路径不安全：{name}")
            if any(part in {".tools", ".pio", "node_modules", ".test-tools", ".test-artifacts",
                            "__pycache__", ".arduino-dependency-work", "Arduino旧库备份"}
                   for part in path.parts) or name.endswith((".exe", ".pyc", ".device-key", ".key", ".p12")) \
                    or path.name == "config.private.json" or (path.name.startswith(".env") and path.name != ".env.example"):
                raise RuntimeError(f"不应交付本机缓存或私有文件：{name}")
        if z.read("VERSION").decode("utf-8").strip() != version():
            raise RuntimeError("包内版本与当前 VERSION 不一致")
        major, minor, _ = version().split(".")
        for required in ("CombatBotArduino/CombatBotArduino.ino", "CHANGELOG.md",
                         f"docs/releases/V{major}.{minor}.md", "安装Arduino依赖.ps1", "server/README.md"):
            if required not in names:
                raise RuntimeError(f"缺少交付入口或说明：{required}")
        manifest = json.loads(z.read("源码SHA256.json"))
        extra = set(names) - set(manifest) - {"源码SHA256.json"}
        if any(not name.startswith("versions/") for name in extra):
            raise RuntimeError(f"未登记的交付条目：{sorted(extra)}")
        if extra:
            history = json.loads(z.read("versions/SOURCE_MANIFEST.json"))
            expected_history = {"versions/README.md"}
            for item in history["versions"].values():
                for relative, digest in item["files"].items():
                    name = item["path"] + "/" + relative
                    expected_history.add(name)
                    if sha(z.read(name)) != digest:
                        raise RuntimeError(f"包内历史快照与原始清单不一致：{name}")
            if extra != expected_history:
                raise RuntimeError("包内历史文件集合缺失或含未登记文件")
        for name, digest in manifest.items():
            local = ROOT / "web/index.html" if name == "网页源码/index.html" else DELIVERY / name
            if sha(z.read(name)) != digest or sha(local.read_bytes()) != digest:
                raise RuntimeError(f"交付条目与本地清单不一致：{name}")
        for spec in dependency_specs():
            name = f"Arduino依赖库/{spec['name']}-{spec['version']}.zip"
            with ZipFile(io.BytesIO(z.read(name))) as dependency:
                verify_library_archive(dependency, spec, evidence["dependencies"][spec["name"]])
        if json.loads(z.read("combatbot/test-artifacts/release-evidence.json")) != evidence:
            raise RuntimeError("包内验证凭据与当前成功凭据不一致")
        for name, digest in evidence["sources"].items():
            if sha(z.read(name)) != digest:
                raise RuntimeError(f"验证输入与包内源码不一致：{name}")
        for item in evidence["checks"].values():
            if sha(z.read("combatbot/" + item["log"])) != item["sha256"]:
                raise RuntimeError("包内验证日志哈希不一致")
        core2 = z.read("验证日志/ArduinoESP32-2.0.17.log").decode("utf-8-sig")
        native_name = evidence["builds"]["core3"]["buildName"]
        native = z.read(f"验证日志/ArduinoESP32-3.3.12-{native_name}.log").decode("utf-8")
        for alias, check in ((core2, "core2"), (native, "core3-native")):
            original = z.read("combatbot/" + evidence["checks"][check]["log"]).decode("utf-8-sig")
            if alias != original.replace("\r\n", "\n").replace("\r", "\n"):
                raise RuntimeError("编译日志别名不属于本次凭据")
        if "[SUCCESS]" not in core2 or not native.rstrip().endswith("[exit code: 0]"):
            raise RuntimeError("双核心版本编译成功记录缺失")
        if "FQBN: " + evidence["builds"]["core3"]["fqbn"] + "\n" not in native:
            raise RuntimeError("原生编译分区与凭据不一致")
        header = z.read("CombatBotArduino/web_asset.h").decode("utf-8")
        embedded = bytes(int(v, 16) for v in re.findall(r"0x([0-9a-f]{2})", header))
        if gzip.decompress(embedded) != z.read("combatbot/web/index.html"):
            raise RuntimeError("Arduino 内嵌网页与源码不一致")
        if z.read("网页源码/index.html") != z.read("combatbot/web/index.html"):
            raise RuntimeError("可编辑网页副本不一致")
        print(f"Delivery {version()}: CRC, {len(manifest)} entry hashes, four fixed libraries, evidence and HTML PASS")
        print(f"Archive entries: {len(names)}; bytes: {archive.stat().st_size}")


if __name__ == "__main__":
    verify_archive(DELIVERY / archive_name())
