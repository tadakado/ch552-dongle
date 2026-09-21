/* SPDX-License-Identifier: MIT */
#ifndef NEO_H
#define NEO_H

#include <stdint.h>

/* データ線を出力 (プッシュプル) に設定する */
void neo_init(void);

/*
 * 1 灯ぶんの色を送出する。r/g/b は 0..255 の「色」、
 * level は 0..NEO_MAX の「明るさ」で、内部で乗算して合成する。
 * 送出中は割り込みを止めるため、24bit = 約 30us のあいだ
 * USB 割り込みとタイマ割り込みが遅延する。実害の出る長さではない。
 */
void neo_show(uint8_t r, uint8_t g, uint8_t b, uint8_t level);

/* 続けて 2 回 neo_show() する場合に、あいだへ挟むリセット待ち */
void neo_latch(void);

#endif /* NEO_H */
