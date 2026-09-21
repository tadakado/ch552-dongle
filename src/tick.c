/* SPDX-License-Identifier: MIT */
#include "ch552.h"
#include "config.h"
#include "tick.h"

volatile uint32_t tick_ms = 0;

/*
 * Timer2 の入力は既定で Fsys/12 (T2MOD の bT2_CLK = 0)。
 * 24MHz なら 2MHz なので 1ms = 2000 カウント。
 *
 * CP_RL2 = 0 の 16bit オートリロードにしてあるので、溢れた瞬間に
 * ハードウェアが RCAP2H:RCAP2L を TH2:TL2 へ入れ直す。ISR は
 * フラグを落として数えるだけでよく、割り込みの遅れが周期に効かない。
 *
 * ソフトで再ロードする作りにすると、ISR に入るまでに進んだぶんを
 * 毎回捨てることになり、しかも捨てる向きが常に同じなので誤差が
 * 片側に溜まる。実測で 0.29% (1 分あたり 174ms) 遅れていた。
 * neo_show() が 33us ほど EA を落とすのがその半分以上を占めていた。
 *
 * TF2 はハードウェアでは落ちないので ISR で必ずクリアすること。
 * また RCLK / TCLK が 1 だと TF2 自体が立たなくなる仕様なので、
 * T2CON では両方 0 にしてある。
 */
/*
 * 四捨五入する。切り捨てると 16MHz で 1333.33 -> 1333 になり、
 * 1 周期が 999.75us、1 日で 21.6 秒ずれる。24MHz (2000) と
 * 12MHz (1000) は割り切れるので影響しない。
 */
#define T2_TICKS  ((FREQ_SYS / 12UL + 500UL) / 1000UL)
#define T2_RELOAD (65536UL - T2_TICKS)

void tick_init(void)
{
    T2CON = 0x00;   /* タイマ動作、オートリロード (CP_RL2 = 0) */
    T2MOD = 0x00;
    RCAP2H = (uint8_t)(T2_RELOAD >> 8);
    RCAP2L = (uint8_t)(T2_RELOAD & 0xFF);
    TH2 = RCAP2H;
    TL2 = RCAP2L;
    TF2 = 0;
    ET2 = 1;
    TR2 = 1;
}

void tick_isr(void) __interrupt(INT_NO_TMR2)
{
    TF2 = 0;    /* Timer2 の溢れフラグはハードで落ちない */
    tick_ms++;
}

uint32_t tick_now(void)
{
    uint32_t v;
    uint8_t saved_ea = EA;

    EA = 0;
    v = tick_ms;
    EA = saved_ea;
    return v;
}

uint16_t tick_now16(void)
{
    uint16_t v;
    uint8_t saved_ea = EA;

    EA = 0;
    v = (uint16_t)tick_ms;
    EA = saved_ea;
    return v;
}
