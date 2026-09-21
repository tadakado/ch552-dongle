/* SPDX-License-Identifier: MIT */
#ifndef TICK_H
#define TICK_H

#include <stdint.h>
#include "ch552.h"

/*
 * Timer2 を 1ms 周期の割り込みに仕立てる。
 * モード B の「1 分ごと」もタッチキーの長押し判定も、
 * すべてこの 1 本の時間軸から数える。
 *
 * Timer0 ではなく Timer2 を使うのは、Timer2 だけが
 * 16bit のハードウェアオートリロードを持っているため。
 * 溢れた瞬間にハードがリロード値を入れ直すので、
 * 割り込みがいつ走ろうと周期がずれない。
 */
void tick_init(void);

/* 起動からの経過ミリ秒。ISR と本体で共有するので volatile */
extern volatile uint32_t tick_ms;

/*
 * ISR のプロトタイプは main.c を含むモジュールから見えている必要がある。
 * SDCC は main のあるファイルで宣言を見ないと割り込みベクタを張らない。
 */
void tick_isr(void) __interrupt(INT_NO_TMR2);

/* tick_ms を割り込みから守って読む (32bit なので非アトミック) */
uint32_t tick_now(void);

/*
 * 下位 16bit だけを読む。65.536 秒で一周するので、それより短い
 * 期限にしか使えないが、32bit の一時領域を作らずに済む。
 * iRAM のオーバレイ領域は数バイトしか余っていないので、
 * 短い待ちにはこちらを使う。
 */
uint16_t tick_now16(void);

#endif /* TICK_H */
