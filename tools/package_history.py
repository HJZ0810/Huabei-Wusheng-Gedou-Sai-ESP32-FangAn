"""完整版本资料打包入口。

当前固件仍由原交付工具检查源码、编译日志与资源一致性；本工具负责把
只读历史快照加入同一交付包，并生成可独立下载的旧版工程。历史说明的
补写不改变旧源码，也不构成旧版本重新编译或上板验证的证据。
"""

from pathlib import Path
from zipfile import ZipFile, ZipInfo, ZIP_DEFLATED
import hashlib
import json
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]


def checked_history():
    """按清单校验每个历史文件，并限制读取范围为仓库内的 versions 目录。"""
    manifest = json.loads((ROOT / "versions/SOURCE_MANIFEST.json").read_text(encoding="utf-8"))
    required = {"v1.0", "v2.0", "v3.0"}
    if int((ROOT / "VERSION").read_text(encoding="utf-8").split(".")[0]) >= 5:
        required.add("v4.0")
    if not required.issubset(manifest["versions"]):
        raise ValueError("完整历史交付缺少已发布版本快照")
    histories = []
    for version, item in manifest["versions"].items():
        if not re.fullmatch(r"v\d+\.\d+(?:\.\d+)?", version):
            raise ValueError(f"版本名格式不正确：{version}")
        base = (ROOT / item["path"]).resolve()
        if not base.is_relative_to((ROOT / "versions").resolve()):
            raise ValueError(f"历史目录超出归档范围：{version}")
        actual = {path.relative_to(base).as_posix() for path in base.rglob("*") if path.is_file()}
        if actual != set(item["files"]):
            raise ValueError(f"历史文件集合与清单不一致：{version}，请核对缺失或未登记文件")
        sketch = item.get("arduinoSketch", base.name + ".ino")
        sketch_path = (base / sketch).resolve()
        if sketch not in item["files"] or not sketch_path.is_relative_to(base) or sketch_path.stem != sketch_path.parent.name:
            raise ValueError(f"历史 Arduino 入口无效：{version}/{sketch}")
        files = []
        for name, digest in item["files"].items():
            path = (base / name).resolve()
            if not path.is_relative_to(base) or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
                raise ValueError(f"历史快照与清单不一致：{version}/{name}")
            files.append(path)
        # 在维护仓库中进一步核对原始 Git blob；无 .git 的下载包仍可用 SHA 清单检查。
        if (ROOT / ".git").exists():
            refs = [f"{item['commit']}:{name}" for name in item["files"]]
            result = subprocess.run(["git", "cat-file", "--batch"], cwd=ROOT,
                                    input=("\n".join(refs) + "\n").encode("utf-8"), capture_output=True, check=True)
            position = 0
            for name, digest in item["files"].items():
                end = result.stdout.index(b"\n", position)
                header = result.stdout[position:end].split()
                if len(header) != 3 or header[1] != b"blob":
                    raise ValueError(f"历史来源 Git blob 缺失：{version}/{name}")
                size = int(header[2]); position = end + 1
                blob = result.stdout[position:position + size]; position += size + 1
                if hashlib.sha256(blob).hexdigest() != digest:
                    raise ValueError(f"历史清单与原始提交不一致：{version}/{name}")
        histories.append((version, item, base, files))
    return histories


def write_entry(output, name, data):
    """使用固定时间戳和排序生成可复现归档；不继承本机文件时间。"""
    info = ZipInfo(name, (1980, 1, 1, 0, 0, 0))
    info.compress_type = ZIP_DEFLATED
    output.writestr(info, data)


def main():
    histories = checked_history()
    scripts = ROOT / "combatbot/scripts"
    # 先通过现有门控；缺失或失效的验证凭据不能通过历史打包入口绕过。
    subprocess.run([sys.executable, str(scripts / "package_delivery.py")], check=True)
    version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        raise ValueError("VERSION 应为 major.minor.patch 数字版本")
    major, minor, _ = version.split(".")
    package = ROOT / f"CombatBot_V{major}.{minor}_Arduino完整工程.zip"
    if not package.is_file():
        raise FileNotFoundError(f"当前打包器未生成对应版本的工程包：{package.name}")
    additions = [ROOT / "versions/README.md", ROOT / "versions/SOURCE_MANIFEST.json",
                 ROOT / "AGENTS.md", Path(__file__).resolve()]
    additions += [path for _, _, _, files in histories for path in files]
    with ZipFile(package, "a", ZIP_DEFLATED) as output:
        present = set(output.namelist())
        for path in sorted(additions):
            name = path.relative_to(ROOT).as_posix()
            if name in present:
                if output.read(name) != path.read_bytes():
                    raise ValueError(f"包内条目与维护文件不同：{name}")
                continue
            write_entry(output, name, path.read_bytes())
            present.add(name)
    subprocess.run([sys.executable, str(scripts / "verify_delivery.py")], check=True)
    with ZipFile(package) as check:
        for path in additions:
            assert check.read(path.relative_to(ROOT).as_posix()) == path.read_bytes()

    for version, item, base, files in histories:
        title = version.upper()
        target = ROOT / f"CombatBot_{title}_Archive.zip"
        module_hashes = {f"{base.name}/{name}": digest for name, digest in item["files"].items()}
        note = (f"# {title} 历史工程归档\n\n"
                f"来源提交：{item['commit']}。原始源码、README、许可证和文档逐字节保留。\n\n"
                "本次恢复不包含旧版重新编译或上板验证；原说明中的能力与验证记录属于当时版本。\n\n"
                f"扩写说明：https://github.com/HJZ0810/Huabei-Wusheng-Gedou-Sai-ESP32-FangAn/blob/main/docs/releases/{title}.md\n\n"
                f"Arduino 主入口位于 {base.name}/{item.get('arduinoSketch', base.name + '.ino')}；请保留全部同级模块。\n")
        with ZipFile(target, "w", ZIP_DEFLATED) as output:
            for path in sorted(files):
                write_entry(output, f"{base.name}/{path.relative_to(base).as_posix()}", path.read_bytes())
            write_entry(output, "归档说明.md", note.encode("utf-8"))
            write_entry(output, "SOURCE_SHA256.json", (json.dumps(module_hashes, indent=2, ensure_ascii=False) + "\n").encode("utf-8"))
        with ZipFile(target) as check:
            assert check.testzip() is None
            for name, digest in module_hashes.items():
                assert hashlib.sha256(check.read(name)).hexdigest() == digest
        print(f"Historical archive: {target.name}; {len(files)} original files; CRC/SHA256 PASS")
    print("Current delivery and complete version history: PASS")


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    main()
