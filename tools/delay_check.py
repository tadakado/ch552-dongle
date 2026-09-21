#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""delay_us() の 1 周が本当に 1us になっているかを生成コードから数える。

以前ここは C の二重ループで、「内側 1 周 = 4 クロック」という思い込みの
係数を使っていた。実際は内側 11 クロック + 外側 26 クロックで、24MHz で
3.83 倍、12MHz では 4.92 倍長かった。しかも固定費が支配的なので、
クロックを下げるほど誤差が広がっていた。

同じ間違いを二度としないよう、リストファイルからサイクル数を数え直す。

使い方:
    make            # build/sys.rst を作る
    python3 tools/delay_check.py
"""
import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from ch55x import parse_listing, cycles, read_freq  # noqa: E402

TOLERANCE = 0.05   # 5% 以内なら可とする


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rst", default="build/sys.rst")
    ap.add_argument("--config", default="config.h")
    args = ap.parse_args()

    if not pathlib.Path(args.rst).exists():
        sys.exit(f"{args.rst} がない。先に make すること")

    freq = read_freq(args.config)
    ns = 1e9 / freq
    rows, labels = parse_listing(args.rst, "delay_us")
    if not rows:
        sys.exit("delay_us が見つからない")

    start = labels.get("00001$")
    if start is None:
        sys.exit("delay_us のループラベルが無い。sys.c の形を確認すること")

    # 00001$ から、そこへ戻る最初の djnz までが 1 周。
    body = [r for r in rows if r[0] >= start]
    end = None
    for i, (addr, nb, mnem) in enumerate(body):
        if mnem.lower().startswith(("jnz", "djnz")) \
                and mnem.strip().endswith("00001$"):
            end = i
            break
    if end is None:
        sys.exit("ループの戻り先が見つからない (djnz 形になっているか)")
    loop = body[:end + 1]

    print(f"Fsys = {freq/1e6:g} MHz  (1 サイクル = {ns:.2f} ns)"
          f"  [config.h より]\n")
    print("delay_us の 1 周:")

    total = 0
    for addr, nb, mnem in loop:
        c = cycles(addr, nb, mnem, labels)
        total += c
        print(f"  {addr:04X}  {nb}B  {c:2d}cyc  {mnem}")

    # --- 配置不変であることの確認 ---------------------------------------
    # DJNZ は本命令と分岐先の両方の奇偶が効く。分岐距離が奇数なら、
    # どこに置かれても必ず片方だけが奇数になるので周期が動かない。
    djnz_addr = loop[-1][0]
    distance = djnz_addr - start
    invariant = (distance & 1) == 1
    print(f"\n  分岐距離 {distance} バイト "
          f"({'奇数 -> 配置不変' if invariant else '偶数 -> 配置でぶれる'})")
    if not invariant:
        print("  NG: リンク配置が変わると 1 周が +-1 サイクル動く。"
              "\n      NOP の個数を奇数にすること。")

    want = freq / 1e6
    val = total * ns
    err = (total - want) / want
    ok = abs(err) <= TOLERANCE and invariant
    print(f"\n  1 周 = {total} cyc = {val:.2f} ns"
          f"  (目標 {want:.0f} cyc = 1000 ns, 誤差 {err*100:+.1f}%)"
          f"  {'OK' if ok else 'NG'}")
    print(f"  delay_ms(1)   = {val:.1f} ns x 1000 = {val/1000:.4f} ms")
    print(f"  delay_ms(100) = {val/10:.2f} ms")
    print("\n  ※ 呼び出しごとの前後処理 (push/pop と 16bit の準備) と、"
          "\n    256 反復に 1 回まわる上位側の djnz が別に乗る。"
          "\n    delay_ms(100) に対しては 0.1% 程度で、無視できる。")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
