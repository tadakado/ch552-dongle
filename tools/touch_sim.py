#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""touch.c の状態機械を 1 サンプル = 1ms で模擬して、前段フィルタを比べる。

実機が無いあいだ、タッチの検出漏れ・誤検出を机上で見るための道具。
config.h から定数を読むので、閾値やデバウンスを変えたらここも追随する。

これまでに 2 回、前段フィルタの設計で失敗している。

  1. ベースラインからの距離で外れ値を捨てる
     -> delta が大きい強いタッチを丸ごと捨てる。閾値の上限が到達不能。
  2. 前回の平滑値からの跳びで捨てる
     -> IIR は生値に遅れてついてくるので、速くて強いタッチほど差が開く。
        捨てると平滑値が更新されないので後続も捨て続け、0.2 秒後に
        「指が乗った状態」を較正してしまう。

どちらも「ゆっくり触ると効くのに、しっかり速く触ると効かない」という
同じ形で出る。今は 3 点メディアンにしてある。前段を触るときは、
必ずこれを通してから実機に行くこと。

使い方:
    python3 tools/touch_sim.py
"""
import argparse
import pathlib
import random
import re
import statistics
import sys


def read_defines(path="config.h"):
    text = pathlib.Path(path).read_text(errors="replace")
    out = {}
    for m in re.finditer(r"^#define\s+(TOUCH_\w+)\s+(\d+)", text, re.M):
        out[m.group(1)] = int(m.group(2))
    return out


def run(seq, thr, mode, cfg):
    avg = cfg.get("TOUCH_AVG_SHIFT", 2)
    bshift = cfg.get("TOUCH_BASE_SHIFT", 6)
    bperiod = cfg.get("TOUCH_BASE_PERIOD", 64)
    deb = cfg.get("TOUCH_DEBOUNCE", 3)
    hyst = cfg.get("TOUCH_HYST_SHIFT", 2)
    jump = 500          # 撤去済みの旧ゲート。比較のために残してある
    limit = 200

    base = int(statistics.median(seq[:100]))
    filt = base
    acc = base << avg
    down = streak = outl = bdiv = 0
    press = recal = 0
    holds = []
    t_down = None
    h0 = h1 = h2 = base

    for n, raw in enumerate(seq):
        if mode == "gate":                      # 旧: 跳びで捨てる
            if abs(raw - filt) > jump:
                outl += 1
                if outl >= limit:
                    recal += 1
                    base = filt = raw
                    acc = raw << avg
                    down = outl = 0
                continue
            outl = 0
            x = raw
        elif mode == "none":                    # 前段なし
            x = raw
        elif mode == "median3":                 # 現行: 3 点メディアン
            h0, h1, h2 = h1, h2, raw
            x = sorted((h0, h1, h2))[1]
        else:
            sys.exit(f"unknown mode {mode}")

        acc = acc - (acc >> avg) + x
        filt = acc >> avg
        delta = base - filt if base > filt else 0
        thr_off = thr - (thr >> hyst)

        if not down:
            if delta >= thr:
                streak += 1
                if streak >= deb:
                    down, streak, press, t_down = 1, 0, press + 1, n
            else:
                streak = 0
                bdiv += 1
                if bdiv >= bperiod:
                    bdiv = 0
                    if filt > base:
                        base += ((filt - base) >> bshift) + 1
                    elif filt < base:
                        base -= ((base - filt) >> bshift) + 1
        else:
            if delta <= thr_off:
                streak += 1
                if streak >= deb:
                    down, streak = 0, 0
                    holds.append(n - t_down)
            else:
                streak = 0
    return press, recal, (holds[0] if holds else None)


def finger(delta, land, hold, noise=3, glitch=None, base=10000, n=3000):
    """base から delta だけ下がる指。land ms で置き、hold ms 保持して離す。"""
    seq = []
    for k in range(n):
        t = k - 500
        if t < 0:
            d = 0
        elif t < land:
            d = delta * t / land
        elif t < land + hold:
            d = delta
        elif t < land + hold + land:
            d = delta * (1 - (t - land - hold) / land)
        else:
            d = 0
        seq.append(int(base - d + random.gauss(0, noise)))
    if glitch:
        # WS2812 の送出 (32us) と重なった 1 サンプルだけが飛ぶ様子
        for k in range(0, n, 20):
            seq[k] += glitch * random.choice((1, -1))
    return seq


CASES = [
    ("ゆっくり Δ=600 を 50ms で置く",   dict(delta=600, land=50, hold=300)),
    ("速い Δ=600 を 4ms で置く",        dict(delta=600, land=4, hold=150)),
    ("しっかり Δ=2000 を 12ms で置く",  dict(delta=2000, land=12, hold=1500)),
    ("強く Δ=4000 を 25ms で置く",      dict(delta=4000, land=25, hold=1500)),
    ("触らず glitch ±1500",             dict(delta=0, land=1, hold=1, glitch=1500)),
    ("触らず glitch ±3000",             dict(delta=0, land=1, hold=1, glitch=3000)),
    ("Δ=600 ゆっくり + glitch ±3000",   dict(delta=600, land=50, hold=300, glitch=3000)),
    ("Δ=4000 を 25ms + glitch ±3000",   dict(delta=4000, land=25, hold=1500, glitch=3000)),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--thr", type=int, default=300)
    ap.add_argument("--config", default="config.h")
    args = ap.parse_args()
    cfg = read_defines(args.config)

    modes = ("gate", "none", "median3")
    print(f"閾値 {args.thr} / IIR 1/{1 << cfg.get('TOUCH_AVG_SHIFT', 2)} / "
          f"デバウンス {cfg.get('TOUCH_DEBOUNCE', 3)} "
          f"[config.h より]\n")
    print(f"{'':34s} | " + " | ".join(f"{m:>22s}" for m in
                                      ("旧ゲート", "前段なし", "3点メディアン")))
    print("-" * 110)
    bad = 0
    for name, kw in CASES:
        row = []
        for m in modes:
            random.seed(1)
            p, r, h = run(finger(**kw), args.thr, m, cfg)
            row.append(f"押下{p} 再較正{r} 保持{h if h else '-'}")
            if m == "median3":
                want_press = 0 if kw["delta"] == 0 else 1
                if p != want_press or r != 0:
                    bad += 1
        print(f"{name:34s} | " + " | ".join(f"{c:>22s}" for c in row))
    print()
    if bad:
        print(f"  NG: 3 点メディアンで想定と違う結果が {bad} 件")
        return 1
    print("  3 点メディアンは全ケースで想定どおり "
          "(指は必ず 1 回検出、glitch だけでは誤検出せず、再較正も起きない)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
