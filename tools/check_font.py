#!/usr/bin/env python3
"""检查界面代码里用到的汉字是否都在字库里，缺字会显示成方块。

用法： python3 tools/check_font.py
退出码 0 = 全覆盖；1 = 有缺字（会列出字符和码点）
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCES = ["main/ui/ui_main.c", "main/main.c"]
FONTS = [ROOT / "main/fonts/lv_font_misans_16.c",
         ROOT / "main/fonts/lv_font_misans_24.c"]


def strip_comments(s: str) -> str:
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
    return re.sub(r"//[^\n]*", "", s)


def main() -> int:
    used = {}
    for rel in SOURCES:
        src = strip_comments((ROOT / rel).read_text(encoding="utf-8"))
        for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', src):
            for ch in lit:
                # 任何非 ASCII 字符都要在字库里：之前只查 CJK 区，
                # 结果间隔号 "·"(U+00B7) 这种标点漏掉，屏幕上就是方块
                if ord(ch) > 0x7F:
                    used.setdefault(ch, rel)

    bad = 0
    for font in FONTS:
        cps = {int(m, 16) for m in re.findall(r"/\* U\+([0-9A-Fa-f]{4})",
                                              font.read_text(encoding="utf-8"))}
        missing = sorted(ch for ch in used if ord(ch) not in cps)
        print(f"{font.name}: 界面用字 {len(used)} 个，覆盖 {len(used) - len(missing)} 个")
        if missing:
            print("  缺字：" + " ".join(f"{c}(U+{ord(c):04X})" for c in missing))
            bad = 1

    if bad:
        print("→ 把缺的字补进 tools/gen_fonts.sh 的 CHARS，再跑 ./tools/gen_fonts.sh")
    return bad


if __name__ == "__main__":
    sys.exit(main())
