#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""src/breathe_lut.h を生成する。

呼吸パターンは「知覚上の明るさ」を cos の半波で振ってから
ガンマ 2.2 を掛けて WS2812 のデューティに変換している。
そのまま cos を PWM 値にすると、暗い側が一瞬で通り過ぎて
明るい側に張り付いた、ぼんやりとは程遠い点滅になる。
"""
import math
import pathlib

N = 64        # 位相の分割数。main.c 側で & (N-1) するので 2 の冪であること
MAX = 0x40    # 最大輝度
GAMMA = 2.2

vals = [min(MAX, round(MAX * (((1 - math.cos(2 * math.pi * i / N)) / 2) ** GAMMA)))
        for i in range(N)]

rows = ["    " + ", ".join(f"0x{v:02X}" for v in vals[r:r + 8]) + ","
        for r in range(0, N, 8)]

out = pathlib.Path(__file__).resolve().parent.parent / "src" / "breathe_lut.h"
out.write_text(
    "/* SPDX-License-Identifier: MIT */\n"
    "/*\n"
    " * 呼吸パターン輝度テーブル（tools/gen_lut.py が生成。手で編集しないこと）\n"
    f" *   位相 {N} 分割、知覚上の明るさを cos 半波で振り、ガンマ {GAMMA} を掛けて\n"
    f" *   実際の WS2812 デューティに変換したもの。最大値は NEO_MAX(0x{MAX:02X})。\n"
    " *   人間の目には、暗い側で長く留まり明るい側で滑らかに折り返す。\n"
    " */\n"
    "#ifndef BREATHE_LUT_H\n#define BREATHE_LUT_H\n\n"
    f"#define BREATHE_STEPS {N}\n\n"
    "static __code const unsigned char breathe_lut[BREATHE_STEPS] = {\n"
    + "\n".join(rows) + "\n};\n\n#endif /* BREATHE_LUT_H */\n")
print(f"wrote {out} (peak {max(vals)})")
