#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""割り込みの呼び出し木がオーバレイ領域 (OSEG) を使っていないか調べる。

SDCC は非再入関数の引数とローカルを OSEG に置き、リンカは全モジュールの
OSEG を **同じ番地に重ねる** (OVR)。呼び出し木が交わらない関数どうしは
それで構わないが、ISR から呼ばれる関数だけは別で、メインが走らせている
関数の引数を割り込みが踏み潰す。

実際に踏んでいた:
  usb.c の copy_code(dst, src, n) の引数が 0x68 に置かれ、そこは
  ライブラリの __divulong_PARM_2 / __divuint_PARM_2 / __gptrput_PARM_2 と
  同じ番地だった。main.c の proximity_level() が 32bit 除算をしている
  最中に GET_DESCRIPTOR が来ると、除数が記述子のポインタに化ける。
  除算ルーチンは除数を計算ループの中で読み直すので、窓は呼び出しの
  一瞬ではなく 100us 規模になる。

見つけ方はアセンブリの呼び出し関係から。ISR を根として辿り、
届いた関数が OSEG に何か置いていたら NG。ライブラリ関数
(__divuint など) も引数を OSEG に置くので、呼んでいたら NG。

使い方:
    make
    python3 tools/oseg_check.py
"""
import pathlib
import re
import sys


def isr_roots(srcdir="src"):
    """src/*.c から __interrupt 関数の名前を拾う"""
    names = []
    for c in sorted(pathlib.Path(srcdir).glob("*.c")):
        for m in re.finditer(r"(\w+)\s*\([^;{)]*\)\s*__interrupt", c.read_text(errors="replace")):
            names.append(m.group(1))
    return names


def call_graph(builddir="build"):
    """build/*.asm から「関数 -> 呼ぶ関数」を作る"""
    graph = {}
    for a in sorted(pathlib.Path(builddir).glob("*.asm")):
        cur = None
        for line in a.read_text(errors="replace").splitlines():
            m = re.match(r"^_(\w+):\s*$", line)
            if m:
                cur = m.group(1)
                graph.setdefault(cur, set())
                continue
            if cur is None:
                continue
            m = re.search(r"\b(?:lcall|ljmp)\s+_+(\w+)", line)
            if m:
                graph[cur].add(m.group(1))
    return graph


def oseg_owners(builddir="build"):
    """OSEG に置かれている記号 -> それを持つ関数名"""
    owners = {}
    for r in sorted(pathlib.Path(builddir).glob("*.rst")):
        inside = False
        for line in r.read_text(errors="replace").splitlines():
            if re.search(r"\.area\s+OSEG", line):
                inside = True
                continue
            if re.search(r"\.area\s+(?!OSEG)\w+", line):
                inside = False
            if not inside:
                continue
            m = re.search(r"^\s*[0-9A-F]{6}\s+\d+\s+_(\w+):", line)
            if m:
                sym = m.group(1)
                fn = re.sub(r"_(PARM_\d+|sloc\d+.*)$", "", sym)
                owners.setdefault(fn, []).append(sym)
    return owners


def lib_oseg_users(mapfile="build/fw.map"):
    """fw.map の OSEG に居るライブラリ記号 (__divuint_PARM_2 など) の関数名"""
    names = set()
    inside = False
    for line in pathlib.Path(mapfile).read_text(errors="replace").splitlines():
        if re.match(r"^OSEG\s+[0-9A-F]{8}", line):
            inside = True
            continue
        if inside and re.match(r"^\S+\s+[0-9A-F]{8}\s+[0-9A-F]{8}", line):
            inside = False
        if inside:
            m = re.search(r"[0-9A-F]{8}\s+_*(\w+?)_PARM_\d+\s", line)
            if m:
                names.add(m.group(1))
    return names


def main():
    roots = isr_roots()
    if not roots:
        sys.exit("__interrupt 関数が見つからない")

    graph = call_graph()
    owners = oseg_owners()
    libs = lib_oseg_users()

    # ISR から届く関数を全部集める
    seen = set()
    stack = list(roots)
    while stack:
        fn = stack.pop()
        if fn in seen:
            continue
        seen.add(fn)
        stack.extend(graph.get(fn, ()))

    print("割り込みの呼び出し木 (%s から %d 関数):" % ("/".join(roots), len(seen)))

    bad = []
    for fn in sorted(seen):
        if fn in owners:
            bad.append((fn, "OSEG に " + ", ".join(sorted(owners[fn]))))
        if fn in libs:
            bad.append((fn, "引数を OSEG に置くライブラリ関数"))

    if not bad:
        print("  OSEG を使う関数は無い  OK")
        if owners:
            print("  (自前コードで OSEG を使うのは: %s。"
                  " どれも ISR からは届かない)" % ", ".join(sorted(owners)))
        else:
            print("  (自前コードで OSEG を使う関数はひとつも無い)")
        if libs:
            print("  (メイン側が使う OSEG 持ちライブラリ: %s)"
                  % ", ".join(sorted(libs)))
        return 0

    for fn, why in bad:
        print("  %-24s %s  NG" % (fn, why))
    print()
    print("  ISR の呼び出し木が OSEG に触れている。メイン側が使う")
    print("  ライブラリ関数 (%s) と同じ番地なので、" % ", ".join(sorted(libs)))
    print("  引数をファイル内 static に移すか、汎用ポインタをやめること。")
    return 1


if __name__ == "__main__":
    sys.exit(main())
