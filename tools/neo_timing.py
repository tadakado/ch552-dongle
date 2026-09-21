#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""WS2812 のビットタイミングを、実際に生成されたコードから計算する。

周波数は config.h の FREQ_SYS を読む (二重管理をしない)。

使い方:
    make            # build/neo.rst を作る
    python3 tools/neo_timing.py
"""
import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from ch55x import parse_listing, cycles, read_freq  # noqa: E402

# WS2812C-2020 のタイミング仕様 (ns)。Worldsemi のデータシートより。
# WS2812B の「中心値 +-150ns」という書き方とは別物で、こちらは
# 上下限が直接与えられている。以前ここに B の値を書いていて、
# T1L の下限を 3ns しか満たさない設定を「OK」と言っていた。
T0H = (220, 380)
T1H = (580, 1000)
T0L = (580, 1000)
T1L = (580, 1000)
PERIOD_NS = 1250        # 800kbps の公称値
RESET_NS = 280_000      # C-2020 は 280us 以上。B の 50us より長い
MAX_DATA_LOW_NS = 5_000 # これを超えるとフレームが切れたと見なされる

# 規格の内側にこれだけの余裕が無ければ警告する (落としはしない)。
#
# 各区間はピンを変化させる命令どうしの差なので、「ピンが命令の
# どこで変化するか」の不定性は引き算で相殺され、机上値はかなり正確。
# 残るのは内蔵発振器の誤差と、実機の立ち上がり/立ち下がり。
# それでも数十 ns の余裕は持っておきたい。24MHz の 1 サイクルぶんを
# 目安に、絶対値で 40ns としてある (周波数で変えると、16MHz で
# 到達不可能な要求になる)。
MARGIN_WARN_NS = 40


def check(name, cyc, ns, lo, hi, results, warns):
    """規格外なら NG。規格内でも余裕が乏しければ WARN。"""
    val = cyc * ns
    slack = min(val - lo, hi - val)
    good = slack >= 0
    if not good:
        mark = "NG"
    elif slack < MARGIN_WARN_NS:
        mark = "WARN"
        warns.append(name)
    else:
        mark = "OK"
    results.append(good)
    print(f"  {name:6s} {cyc:3d} cyc = {val:7.1f} ns   "
          f"(規格 {lo}-{hi} ns, 余裕 {slack:+.0f}ns)  {mark}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rst", default="build/neo.rst")
    ap.add_argument("--config", default="config.h")
    args = ap.parse_args()

    if not pathlib.Path(args.rst).exists():
        sys.exit(f"{args.rst} がない。先に make すること")

    freq = read_freq(args.config)
    ns = 1e9 / freq
    rows, labels = parse_listing(args.rst, "neo_send_byte")
    if not rows:
        sys.exit("neo_send_byte が見つからない")

    print(f"Fsys = {freq/1e6:g} MHz  (1 サイクル = {ns:.2f} ns)"
          f"  [config.h より]\n")
    print("生成されたビットループ:")

    loop_start = labels.get("00001$")
    body = [r for r in rows if loop_start is None or r[0] >= loop_start]
    head = [r for r in rows if loop_start is not None and r[0] < loop_start]

    idx_setb = idx_mov = idx_clr = idx_djnz = None
    for i, (addr, nb, mnem) in enumerate(body):
        low = mnem.lower()
        if low.startswith("setb") and idx_setb is None:
            idx_setb = i
        elif low.startswith("mov") and low.endswith(", c") and idx_mov is None:
            idx_mov = i
        elif low.startswith("clr") and idx_clr is None:
            idx_clr = i
        elif low.startswith("djnz"):
            idx_djnz = i
    if None in (idx_setb, idx_mov, idx_clr, idx_djnz):
        sys.exit("ビットループの形が想定と違う。neo.c を確認すること")

    for i, (addr, nb, mnem) in enumerate(body):
        c = cycles(addr, nb, mnem, labels)
        mark = ""
        if i == idx_setb:
            mark = "  <- 立ち上がり"
        elif i == idx_mov:
            mark = "  <- 0 ならここで立ち下がる (T0H 確定)"
        elif i == idx_clr:
            mark = "  <- 1 ならここで立ち下がる (T1H 確定)"
        print(f"  {addr:04X}  {nb}B  {c:2d}cyc  {mnem}{mark}")

    def span(rows_, labels_, i0, i1, taken=True):
        """命令 i0 の完了から i1 の完了まで"""
        return sum(cycles(*rows_[k], labels_, taken)
                   for k in range(i0 + 1, i1 + 1))

    t0h = span(body, labels, idx_setb, idx_mov)
    t1h = span(body, labels, idx_setb, idx_clr)
    # 周期: setb の完了から次の setb の完了まで (djnz は成立側)
    period = span(body, labels, idx_setb, idx_djnz) + \
        sum(cycles(*body[k], labels) for k in range(0, idx_setb + 1))

    # --- 配置不変であることの確認 ---------------------------------------
    # djnz は本命令と分岐先の両方の奇偶が効くので、分岐距離が奇数なら
    # どこに置かれても必ず片方だけが奇数になり、周期が動かない。
    distance = body[idx_djnz][0] - loop_start
    invariant = (distance & 1) == 1

    print("\n結果:")
    results = []
    warns = []
    print(f"  {'分岐距離':6s} {distance:3d} B   "
          f"{'奇数 -> 配置不変' if invariant else '偶数 -> 配置でぶれる'}  "
          f"{'OK' if invariant else 'NG'}")
    results.append(invariant)
    check("T0H", t0h, ns, T0H[0], T0H[1], results, warns)
    check("T1H", t1h, ns, T1H[0], T1H[1], results, warns)
    check("T0L", period - t0h, ns, T0L[0], T0L[1], results, warns)
    check("T1L", period - t1h, ns, T1L[0], T1L[1], results, warns)
    print(f"  {'周期':6s} {period:3d} cyc = {period*ns:7.1f} ns   "
          f"(公称 {PERIOD_NS} ns。0 と 1 で同じ長さにしてある)")

    # --- バイト境界の低区間 ---------------------------------------------
    # 8bit 目の djnz が不成立で抜け、pop/ret し、呼び出し側が次のバイトを
    # 積んで再突入し、最初の setb で立ち上がるまで。ここが 280us を
    # 超えるとフレームが切れて、色がずれたまま固まる。
    tail = span(body, labels, idx_clr, idx_djnz - 1) if idx_djnz > idx_clr else 0
    tail += cycles(*body[idx_djnz], labels, taken=False)
    tail += sum(cycles(*r, labels) for r in rows if r[0] > body[idx_djnz][0])
    entry = sum(cycles(*r, labels) for r in head) + \
        span(body, labels, -1, idx_setb)

    caller, clabels = parse_listing(args.rst, "neo_show")
    between = 0
    if caller:
        lcalls = [i for i, r in enumerate(caller)
                  if r[2].lower().startswith("lcall")]
        if len(lcalls) >= 2:
            between = sum(cycles(*caller[k], clabels)
                          for k in range(lcalls[0] + 1, lcalls[1] + 1))
    gap = tail + between + entry
    good = gap * ns <= MAX_DATA_LOW_NS
    results.append(good)
    print(f"  {'バイト間':6s} {gap:3d} cyc = {gap*ns:7.1f} ns   "
          f"(上限 {MAX_DATA_LOW_NS} ns, リセット {RESET_NS} ns)  "
          f"{'OK' if good else 'NG'}")

    if warns:
        print(f"\n  ※ 余裕が {MARGIN_WARN_NS}ns 未満の項目がある: "
              f"{', '.join(warns)}"
              "\n    規格内ではあるが、実機で外れたときに詰める余地が無い。"
              "\n    NOP 数を振り直すことを勧める。")
    print("\n  ※ 各区間はピンを変化させる命令どうしの差なので、"
          "\n    「命令のどこでピンが変わるか」の不定性は相殺される。"
          "\n    それでも最終的にはロジアナで実測すること。")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
