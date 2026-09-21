#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""__naked 関数が壊すレジスタを自分で退避しているかを確かめる。

SDCC は呼び出し先が使うレジスタを iCode から求めるので、本体が
__asm だけの __naked 関数は「レジスタを一つも使わない」と見なされる。
呼び出し側は退避コードを出さないから、そこで壊したレジスタは
呼び出し側の生きた値を巻き込む。

実際に踏んだ: neo_show() が赤成分を R7 に置いたまま緑を送りに行き、
neo_send_byte() の djnz r7 が R7 をちょうど 0 にして返すため、
赤が常に 0 になっていた。モード B (FF,00,00) が完全に消灯し、
白はシアン、マゼンタは青になる。緑と青は正しく出るので気づきにくい。

同じことを二度としないよう、生成コードを機械的に見る。

使い方:
    make
    python3 tools/naked_check.py
"""
import pathlib
import re
import sys

def find_targets(srcdir="src", builddir="build"):
    """src/*.c から __naked 関数を拾って (アセンブリ, 関数名) を返す。

    以前はここに一覧を手で書いていたが、それだと __naked を足したときに
    書き足し忘れれば検査をすり抜ける。検査の意味が半分無くなるので、
    ソースから引くようにした。
    """
    out = []
    for c in sorted(pathlib.Path(srcdir).glob("*.c")):
        text = c.read_text(errors="replace")
        for m in re.finditer(r"(\w+)\s*\([^;{)]*\)\s*__naked", text):
            out.append((f"{builddir}/{c.stem}.asm", m.group(1)))
    return out

# 書き込み先として現れても退避が要らないもの (どこにも値を持ち越さない)
SCRATCH = {"_P1_1", "_P1_0", "_P1_2", "_P1_3", "_P1_4", "_P1_5",
           "_P1_6", "_P1_7"}

# push の名前 -> 実際に守られるレジスタ名
PUSH_COVERS = {
    "acc": {"a", "acc"},
    "psw": {"psw", "c"},
    "b": {"b"},
    "dpl": {"dpl"},
    "dph": {"dph"},
}
for _n in range(8):
    PUSH_COVERS[f"ar{_n}"] = {f"r{_n}", f"ar{_n}"}

# 破壊するがフラグしか触らない命令
NO_DEST = {"push", "pop", "djnz", "ret", "reti", "nop", "sjmp", "ljmp",
           "ajmp", "jz", "jnz", "jc", "jnc", "jb", "jnb", "jbc", "cjne",
           "setb", "clr", "cpl", "rlc", "rrc", "rl", "rr", "lcall",
           "acall", "movc", "swap"}


def check(asm_path, func):
    path = pathlib.Path(asm_path)
    if not path.exists():
        print(f"  {asm_path} がない。先に make すること")
        return False
    text = path.read_text(errors="replace")
    key = f"_{func}:"
    if key not in text:
        print(f"  {func} が {asm_path} に無い")
        return False
    body = text[text.index(key) + len(key):]
    m = re.search(r"^\tret\b", body, re.M)
    if m:
        body = body[:m.end()]

    saved = set()
    for name in re.findall(r"^\tpush\s+(\w+)", body, re.M):
        saved |= PUSH_COVERS.get(name, {name})

    dest = set()
    for line in body.splitlines():
        m2 = re.match(r"^\t(\w+)\s+([^,;\n]+)", line)
        if not m2:
            continue
        op, d = m2.group(1).lower(), m2.group(2).strip()
        if op in NO_DEST:
            # clr/setb/cpl/rlc/rrc は A や C を壊す
            if op in ("clr", "cpl", "rlc", "rrc", "rl", "rr", "swap") \
                    and d.lower() in ("a", "c"):
                dest.add(d.lower())
            elif op in ("setb", "clr", "cpl") and d.lower() == "c":
                dest.add("c")
            elif op == "djnz":
                dest.add(d.strip().lower())
            elif op in ("rlc", "rrc"):
                dest.add("c")
            continue
        dest.add(d.lower())

    # rlc a は C も書き換える
    if re.search(r"^\t(rlc|rrc)\s", body, re.M):
        dest.add("c")
    dest -= {d for d in dest if d.upper() in SCRATCH or d in SCRATCH}

    missing = sorted(d for d in dest if d not in saved)
    ok = not missing
    print(f"  {func:16s} push={sorted(saved)}")
    print(f"  {'':16s} 破壊={sorted(dest)}  "
          f"{'OK' if ok else 'NG (退避漏れ: %s)' % missing}")
    return ok


def main():
    targets = find_targets()
    if not targets:
        print("  src/*.c に __naked 関数が見つからない。検査対象なし")
        return 0
    print(f"__naked 関数のレジスタ退避 ({len(targets)} 個をソースから検出):")
    ok = True
    for asm, func in targets:
        ok &= check(asm, func)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
