"""
CombatBot · 交付包完整性核验
============================================================================
检查：ZIP 条目 CRC、模块 SHA256、依赖库存在性与缓存排除规则。
资源：还原内嵌 gzip 数据，与网页源文件逐字节比较，防止导出资源过期。
边界：此工具只校验交付内容，不替代编译、浏览器兼容性或装机测试。
============================================================================
"""
from pathlib import Path
from zipfile import ZipFile
import hashlib,json,gzip,re,io
from release_evidence import verify_evidence
from verify_arduino_cli import verify_export
evidence=verify_evidence()
verify_export()
base=Path(__file__).resolve().parents[2]
archive=base/"CombatBot_V3.0_Arduino完整工程.zip"
with ZipFile(archive) as z:
    # 先验证压缩包可读，再检查各模块是否与交付时生成的清单完全一致。
    assert z.testzip() is None
    names=z.namelist()
    assert "CombatBotArduino/CombatBotArduino.ino" in names
    assert z.read("VERSION").decode("utf-8").strip()=="3.0.0"
    assert "CHANGELOG.md" in names and "docs/releases/V3.0.md" in names
    assert not any("/.tools/" in n or "/.pio/" in n or n.endswith(".exe") for n in names)
    manifest=json.loads(z.read("源码SHA256.json"))
    for name,digest in manifest.items():
        assert hashlib.sha256(z.read(name)).hexdigest()==digest,name
        assert hashlib.sha256((base/name).read_bytes()).hexdigest()==digest,f"Local delivery differs: {name}"
    for name in ("ArduinoJson-7.3.1.zip","AsyncTCP-3.3.2.zip","ESPAsyncWebServer-3.6.0.zip"):
        assert "Arduino依赖库/"+name in names
        library,version=name.removesuffix(".zip").rsplit("-",1)
        with ZipFile(io.BytesIO(z.read("Arduino依赖库/"+name))) as dependency:
            assert dependency.testzip() is None,name
            assert f"version={version}" in dependency.read(f"{library}/library.properties").decode("utf-8").splitlines(),name
    assert "安装Arduino依赖.ps1" in names
    assert "[SUCCESS]" in z.read("验证日志/ArduinoESP32-2.0.17.log").decode("utf-8-sig")
    assert z.read("验证日志/ArduinoESP32-3.3.12.log").decode("utf-8").rstrip().endswith("[exit code: 0]")
    assert json.loads(z.read("combatbot/test-artifacts/release-evidence.json"))==evidence
    for name,digest in evidence["sources"].items():
        target="网页源码/index.html" if name=="web/index.html" else "combatbot/"+name
        assert hashlib.sha256(z.read(target)).hexdigest()==digest,target
    for item in evidence["checks"].values():
        assert hashlib.sha256(z.read("combatbot/"+item["log"])).hexdigest()==item["sha256"]
    print("Delivery CRC, module checksums, library ZIPs and cache exclusion: PASS")
    print("Arduino modules:",len(manifest),"Archive entries:",len(names),"Bytes:",archive.stat().st_size)
header=(base/"CombatBotArduino"/"web_asset.h").read_text(encoding="utf-8")
# 数据区只包含两位十六进制字节；文件头不应放入形如 0xNN 的示例常量。
data=bytes(int(v,16) for v in re.findall(r"0x([0-9a-f]{2})",header))
assert gzip.decompress(data)==(base/"combatbot"/"web"/"index.html").read_bytes()
print("Embedded gzip matches offline HTML: PASS")
