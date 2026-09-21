/* SPDX-License-Identifier: MIT */
#ifndef SYS_H
#define SYS_H

#include <stdint.h>

/* システムクロックを config.h の FREQ_SYS に設定する */
void sys_init(void);

/* 概算ビジーウェイト。割り込みが動いていると当然伸びる */
void delay_us(uint16_t us);
void delay_ms(uint16_t ms);

/*
 * ブートローダ (0x3800) へ飛ぶ。戻ってこない。
 * USB を止めてから 100ms 待つ手順が肝で、これを省くと
 * ホスト側がデバイスの消失を認識できずに列挙に失敗する。
 */
void sys_jump_bootloader(void);

/*
 * ホストのサスペンド中に眠る (USB のバス活動で起床)。
 * EA = 0 の状態で呼ぶこと。戻るときも EA = 0 のまま。
 * 「眠るかどうか」の判定ごと呼び出し側のクリティカルセクションに
 * 入れないと、判定と実行のあいだのレジュームを取りこぼす。
 */
void sys_sleep(void);

/* ウォッチドッグを有効にする。以後 sys_watchdog_feed() を呼び続けること */
void sys_watchdog_init(void);

/* 餌やり。24MHz では約 700ms 以内に呼び続ける必要がある */
void sys_watchdog_feed(void);

#endif /* SYS_H */
