#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""CH55x の命令周期を .rst (リストファイル) から数えるための共通部品。

WCH の資料にある規則:

  * 非跳転命令の周期数 = 命令バイト数 (CH551/2/3/4 の DIV を除く)
  * DJNZ / JB / JNB / JBC / CJNE は「バイト数 + 2」から。
    本命令のアドレスが奇数なら +1、分岐先が奇数ならさらに +1。
  * 条件分岐が成立しなかった場合はバイト数と同じ。
  * RET/RETI はバイト数 + 3 から。
  * CALL はバイト数 + 2 から (分岐先が奇数なら +1)。
  * MOVC はバイト数 + 4 から。

アドレスとバイト列がリストに出ていれば、NOP を数えなくても
サイクル数が確定する。DJNZ が 1 バイトずれるだけで周期が変わるので、
手計算より機械に数えさせた方がよい。
"""
import pathlib
import re

_LINE_RE = re.compile(r"^\s{0,10}([0-9A-F]{6})\s+((?:[0-9A-F]{2} )+)\s*\[")
_LABEL_RE = re.compile(r"^\s{0,10}([0-9A-F]{6})\s+\d+\s+(\S+):")
_ONLY_LABEL_RE = re.compile(r"^\s{0,10}([0-9A-F]{6})\s+\d+\s+(\S+\$):")


def parse_listing(path, func):
    """.rst から関数 1 つぶんの (アドレス, バイト数, ニモニック) を拾う。"""
    rows = []
    labels = {}
    started = False

    for line in pathlib.Path(path).read_text(errors="replace").splitlines():
        if f"_{func}:" in line:
            started = True
            continue
        if not started:
            continue
        m = _ONLY_LABEL_RE.match(line) or _LABEL_RE.match(line)
        if m and "$" in m.group(2):
            labels[m.group(2).rstrip(":")] = int(m.group(1), 16)
            continue
        m = _LINE_RE.match(line)
        if m:
            addr = int(m.group(1), 16)
            nbytes = len(m.group(2).split())
            rest = line.split("]", 1)[1].split(";")[0].strip()
            mnem = re.sub(r"^\d+\s+", "", rest).strip()
            rows.append((addr, nbytes, mnem))
            if mnem.startswith("ret"):
                break
        elif rows and re.match(r"^\s{0,10}[0-9A-F]{6}\s+\d+\s+_\w+:", line):
            break
    return rows, labels


# 本命令の奇偶も分岐先の奇偶も効く命令
_BOTH_PARITY = ("djnz", "jb", "jnb", "jbc", "cjne")
# 分岐先の奇偶だけが効く命令 (「其余跳转指令」)
_TARGET_PARITY = ("ljmp", "sjmp", "ajmp", "jmp", "jc", "jnc", "jz", "jnz",
                  "lcall", "acall", "call")


def _target_odd(mnem, labels):
    """分岐先ラベルの番地が奇数か。ラベルが分からなければ False。

    SDCC はニーモニックと被演算子を **タブ** で区切る (jnz\t00001$)。
    以前ここで半角スペースだけを見ていたため、`,` を持たない単一被演算子の
    跳転 (jnz / jz / sjmp / lcall) では常に False が返り、
    「其余跳转指令の分岐先奇偶」を一度も数えていなかった。
    str.split() は引数なしなら任意の空白で分けるので、それを使う。
    """
    tgt = mnem.split(",")[-1].split()[-1].strip()
    return tgt in labels and (labels[tgt] & 1) == 1


def cycles(addr, nbytes, mnem, labels, taken=True):
    """CH55x の規則で 1 命令のサイクル数を返す。"""
    op = mnem.split()[0].lower()
    if op in _BOTH_PARITY:
        if not taken:
            return nbytes
        c = nbytes + 2
        if addr & 1:
            c += 1
        if _target_odd(mnem, labels):
            c += 1
        return c
    if op in ("ret", "reti"):
        # 戻り先が奇数ならもう 1 周期。戻り先は呼び出し側で決まるので
        # ここでは数えられない。呼び出し側込みの区間はその 1 周期ぶん
        # 短く出る可能性がある。
        return nbytes + 3
    if op in _TARGET_PARITY:
        if not taken:
            return nbytes
        return nbytes + 2 + (1 if _target_odd(mnem, labels) else 0)
    if op.startswith("movc"):
        return nbytes + 4 + (1 if addr & 1 else 0)
    return nbytes


def read_freq(config="config.h"):
    """config.h の FREQ_SYS を唯一の出所にする。

    Makefile 側にも同じ値を書くと、片方だけ変えたときに
    「実機と検査で周波数が違うのに合格が出る」ことになる。
    """
    text = pathlib.Path(config).read_text(errors="replace")
    m = re.search(r"^#define\s+FREQ_SYS\s+(\d+)", text, re.M)
    if not m:
        raise SystemExit(f"{config} に FREQ_SYS が見つからない")
    return int(m.group(1))
