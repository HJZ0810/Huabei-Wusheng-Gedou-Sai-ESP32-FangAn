"""
CombatBot · Arduino IDE 工程导出
============================================================================
职责：将维护目录的模块源码导出到可直接打开的 CombatBotArduino 草图目录。
顺序：先生成离线网页资源，再复制头文件与模块，最后生成匹配目录名的 .ino。
约定：业务代码保持一致；入口文件的 @file 注释随导出的文件名同步调整。
============================================================================
"""
from pathlib import Path
import shutil
from embed_web import generate, ROOT

generate()
target = ROOT.parent / "CombatBotArduino"
# Windows 不区分大小写，因此导出目录不能仅靠 CombatBot/combatbot 区分。
target.mkdir(exist_ok=True)
for folder, pattern in (("src", "*.cpp"), ("include", "*.h")):
    for source in (ROOT / folder).glob(pattern):
        # Arduino 草图入口与目录同名；其余 .cpp 在 IDE 中独立编译后统一链接。
        if source.name == "main.cpp":
            exported = source.read_text(encoding="utf-8").replace(
                "@file    main.cpp", "@file    CombatBotArduino.ino", 1
            )
            (target / "CombatBotArduino.ino").write_bytes(exported.encode("utf-8"))
        else:
            shutil.copy2(source, target / source.name)
print(f"Arduino sketch: {target}")
