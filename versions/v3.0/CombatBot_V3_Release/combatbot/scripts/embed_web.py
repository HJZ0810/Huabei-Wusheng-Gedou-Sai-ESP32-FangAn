"""
CombatBot · 离线网页嵌入工具
============================================================================
职责：将 web/index.html 压缩为 gzip，并生成可直接编译进 Flash 的 C++ 数据。
入口：支持独立 Python 调用，也支持 PlatformIO 的 pre 构建脚本环境。
约定：固定 gzip 时间戳，避免相同网页因构建时间不同而产生不同的嵌入数据。
============================================================================
"""
from pathlib import Path
import gzip

if "__file__" in globals():
    # 独立运行或模块导入：从脚本位置解析工程根目录，避免依赖当前工作目录。
    ROOT = Path(__file__).resolve().parents[1]
else:
    # SCons 通过脚本环境执行时没有 __file__，从其构建变量取得工程路径。
    Import("env")
    ROOT = Path(env.subst("$PROJECT_DIR"))

def generate():
    """生成网页数据头；文件内容未变化时保留原文件，避免触发额外重编译。"""
    source = ROOT / "web" / "index.html"
    if not source.exists():
        raise FileNotFoundError(source)
    data = gzip.compress(source.read_bytes(), compresslevel=9, mtime=0)
    rows = [", ".join(f"0x{b:02x}" for b in data[i:i+20]) for i in range(0, len(data), 20)]
    # 这里生成的是资源数据，不是业务源码；文件头明确标记维护入口和编辑边界。
    header = (
        "/**\n"
        " * @file    web_asset.h\n"
        " * @brief   离线控制网页的 gzip 资源数据，存放于 Flash。\n"
        " *\n"
        " * ============================================================================\n"
        " * 自动生成\n"
        " *   维护入口：web/index.html；生成工具：scripts/embed_web.py。\n"
        " *   请修改网页源文件后重新生成，不直接编辑下面的十六进制数据。\n"
        " *   HTTP 返回时须设置 Content-Encoding: gzip。\n"
        " * ============================================================================\n"
        " */\n"
        "#pragma once\n#include <Arduino.h>\n"
    )
    header += "static const uint8_t WEB_GZIP[] PROGMEM = {\n" + ",\n".join(rows) + "\n};\n"
    header += f"static const size_t WEB_GZIP_SIZE = {len(data)};\n"
    # 内容一致时不重写，既保留可复现结果，也减少构建缓存的无效失效。
    path = ROOT / "include" / "web_asset.h"
    if not path.exists() or path.read_text(encoding="utf-8") != header:
        path.write_text(header, encoding="utf-8")
    print(f"Embedded web: {source.stat().st_size} -> {len(data)} bytes")

if __name__ == "__main__" or "__file__" not in globals():
    generate()
