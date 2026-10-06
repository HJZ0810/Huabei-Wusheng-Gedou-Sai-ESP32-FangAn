"""交付凭据：绑定当前版本真实源码、服务端、补丁依赖和全部成功日志。"""
from pathlib import Path
from zipfile import ZipFile, ZipInfo, ZIP_DEFLATED
import hashlib
import json
import re

ROOT = Path(__file__).resolve().parents[1]
DELIVERY = ROOT.parent
EVIDENCE = ROOT / "test-artifacts/release-evidence.json"
REQUIRED = {"host", "motor", "ui", "ui-offline", "migration", "core2", "core3",
            "test_session", "test_sensor_model", "core3-native", "v4-control", "arena-compile",
            "arena", "arena-defaults-compile", "arena-defaults", "ui-arena", "server"}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def version():
    value = (DELIVERY / "VERSION").read_text(encoding="utf-8").strip()
    if not re.fullmatch(r"\d+\.\d+\.\d+", value):
        raise RuntimeError("VERSION 必须为 major.minor.patch")
    return value


def archive_name():
    major, minor, _ = version().split(".")
    return f"CombatBot_V{major}.{minor}_Arduino完整工程.zip"


def dependency_specs():
    data = json.loads((DELIVERY / "依赖版本.json").read_text(encoding="utf-8"))
    libraries = data.get("libraries", [])
    if data.get("schemaVersion") != 1 or {i["name"] for i in libraries} != {
            "ArduinoJson", "AsyncTCP", "ESPAsyncWebServer", "WebSockets"}:
        raise RuntimeError("依赖清单必须包含四个固定版本库")
    return libraries


def server_files():
    """白名单不遍历运行配置、node_modules、测试下载或临时凭据。"""
    server = DELIVERY / "server"
    files = [server / name for name in ("package.json", "package-lock.json", ".gitignore", ".env.example")]
    for sub, extensions in (("src", {".mjs"}), ("scripts", {".mjs"}),
                            ("test", {".mjs"}), ("deploy", {".cjs", ".conf", ".service", ".sh"})):
        files.extend(path for path in (server / sub).rglob("*")
                     if path.is_file() and path.suffix in extensions)
    public_root = server / "deploy/isrg-root-x1.crt"
    if public_root.is_file():
        files.append(public_root)
    return sorted(set(files))


def library_files(base):
    return sorted(path for path in base.rglob("*") if path.is_file() and ".git" not in path.parts
                  and path.name not in {".piopm", "CombatBotPatch.json", ".DS_Store"}
                  and "__pycache__" not in path.parts)


def dependency_manifest():
    """绑定实际参与编译的 PIO 库文件；补丁与许可证必须真实存在。"""
    result = {}
    for spec in dependency_specs():
        base = ROOT / ".pio/libdeps/esp32s3" / spec["name"]
        properties = dict(line.split("=", 1) for line in
                          (base / "library.properties").read_text(encoding="utf-8").splitlines()
                          if "=" in line and not line.startswith("#"))
        if properties.get("version") != spec["version"] or not (base / spec["license"]).is_file():
            raise RuntimeError(f"依赖版本或许可证错误：{spec['name']}")
        files = {path.relative_to(base).as_posix(): sha(path.read_bytes()) for path in library_files(base)}
        for name, patch in spec.get("patch", {}).items():
            if files.get(name) != patch["patched"]:
                raise RuntimeError(f"补丁依赖不匹配：{spec['name']}/{name}")
        result[spec["name"]] = {"version": spec["version"], "files": files}
    return result


def create_dependency_archives():
    """固定 ZIP 时间戳与条目顺序，重复生成不会改变交付凭据的输入。"""
    manifest = dependency_manifest()
    directory = DELIVERY / "Arduino依赖库"
    directory.mkdir(exist_ok=True)
    for spec in dependency_specs():
        name = spec["name"]
        base = ROOT / ".pio/libdeps/esp32s3" / name
        target = directory / f"{name}-{spec['version']}.zip"
        with ZipFile(target, "w", ZIP_DEFLATED) as output:
            for path in library_files(base):
                info = ZipInfo((Path(name) / path.relative_to(base)).as_posix(), (1980, 1, 1, 0, 0, 0))
                info.compress_type = ZIP_DEFLATED
                output.writestr(info, path.read_bytes())
            if spec.get("patch"):
                info = ZipInfo(f"{name}/CombatBotPatch.json", (1980, 1, 1, 0, 0, 0))
                info.compress_type = ZIP_DEFLATED
                output.writestr(info, json.dumps(spec, ensure_ascii=False, indent=2).encode("utf-8"))
        verify_library_archive(target, spec, manifest[name])
    return manifest


def verify_library_archive(target, spec, expected):
    # 同时支持磁盘 ZIP 与完整交付包中的嵌套 ZIP。
    from contextlib import nullcontext
    context = nullcontext(target) if isinstance(target, ZipFile) else ZipFile(target)
    with context as archive:
        if archive.testzip() is not None or len(archive.namelist()) != len(set(archive.namelist())):
            raise RuntimeError(f"依赖 ZIP 损坏或重复条目：{target}")
        prefix = spec["name"] + "/"
        hashes = {name[len(prefix):]: sha(archive.read(name)) for name in archive.namelist()
                  if name.startswith(prefix) and name != prefix + "CombatBotPatch.json"}
        if hashes != expected["files"] or any(not name.startswith(prefix) for name in archive.namelist()):
            raise RuntimeError(f"依赖 ZIP 与验证过的实际库不一致：{target}")
        if spec.get("patch") and json.loads(archive.read(prefix + "CombatBotPatch.json")) != spec:
            raise RuntimeError("补丁来源清单不一致")


def prepare_isolated_libraries(explicit=None):
    """仅在本工程 test-artifacts 下准备用户库；不替换全局 Arduino 库。"""
    manifest = dependency_manifest()
    identity = sha(json.dumps(manifest, sort_keys=True).encode())[:12]
    artifacts = (ROOT / "test-artifacts").resolve()
    user = (Path(explicit) if explicit else artifacts / f"arduino-user-{identity}").resolve()
    if not user.is_relative_to(artifacts) or user == artifacts:
        raise RuntimeError("隔离用户库目录必须位于本工程 test-artifacts 的独立子目录")
    libraries = user / "libraries"
    for spec in dependency_specs():
        target = DELIVERY / "Arduino依赖库" / f"{spec['name']}-{spec['version']}.zip"
        verify_library_archive(target, spec, manifest[spec["name"]])
        base = libraries / spec["name"]
        if base.exists():
            actual = {path.relative_to(base).as_posix(): sha(path.read_bytes()) for path in library_files(base)}
            if actual != manifest[spec["name"]]["files"]:
                raise RuntimeError(f"已有隔离库内容不匹配，请选择新的 test-artifacts 子目录：{base}")
            continue
        with ZipFile(target) as archive:
            for name in archive.namelist():
                path = (libraries / name).resolve()
                if not path.is_relative_to(libraries.resolve()):
                    raise RuntimeError("依赖 ZIP 路径越界")
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(archive.read(name))
    return user


def source_manifest():
    """仓库相对路径绑定固件、中继、依赖、工具及版本，文档可最后补写。"""
    files = [ROOT / "platformio.ini", DELIVERY / "VERSION", DELIVERY / "依赖版本.json",
             DELIVERY / "安装Arduino依赖.ps1", DELIVERY / "tools/package_history.py",
             DELIVERY / ".gitignore", DELIVERY / ".gitattributes", DELIVERY / "AGENTS.md",
             DELIVERY / "versions/SOURCE_MANIFEST.json"]
    for name in ("src", "include", "web", "scripts", "test"):
        files.extend(path for path in (ROOT / name).rglob("*") if path.is_file()
                     and "__pycache__" not in path.parts and path.suffix not in (".exe", ".pyc"))
    files += server_files()
    files += [DELIVERY / "Arduino依赖库" / f"{i['name']}-{i['version']}.zip" for i in dependency_specs()]
    return {path.relative_to(DELIVERY).as_posix(): sha(path.read_bytes()) for path in sorted(set(files))}


def verify_evidence():
    data = json.loads(EVIDENCE.read_text(encoding="utf-8"))
    if data.get("schemaVersion") != 2 or data.get("firmwareVersion") != version():
        raise RuntimeError("凭据版本与当前工程不一致，请重新执行 verify_release.py")
    if data.get("sources") != source_manifest() or data.get("dependencies") != dependency_manifest():
        raise RuntimeError("源码、服务端、工具或依赖已改变，请重跑完整验证")
    checks = data.get("checks", {})
    if not REQUIRED.issubset(checks):
        raise RuntimeError("交付凭据缺少必要的软件检查")
    for name, item in checks.items():
        path = (ROOT / item["log"]).resolve()
        if not path.is_relative_to((ROOT / "test-artifacts").resolve()) or item["exitCode"] != 0 or sha(path.read_bytes()) != item["sha256"]:
            raise RuntimeError(f"验证凭据失效：{name}")
    return data
