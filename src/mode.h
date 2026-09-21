/* SPDX-License-Identifier: MIT */
/*
 * mode.h - 動作モードの状態機械
 *
 *   MODE_IDLE    何もしない。USB HID として繋がっているだけ。LED 緑
 *   MODE_WIGGLE  1 分ごとにマウスを右へ 1、さらに 1 分後に左へ 1。LED 赤
 *
 * タッチの短押しで切り替える。長押しは閾値の学習と DFU に割り当てて
 * あるので、短押しだけがモード切替に残っている。
 */
#ifndef MODE_H
#define MODE_H

#include <stdint.h>

typedef enum {
    MODE_IDLE = 0,
    MODE_WIGGLE = 1
} mode_t;

void   mode_init(void);
void   mode_poll(void);
void   mode_toggle(void);
mode_t mode_get(void);

/* 現在のモードの色を返す (R, G, B) */
uint8_t mode_color_r(void);
uint8_t mode_color_g(void);
uint8_t mode_color_b(void);

/* 次にマウスを動かすまでの残り秒数。コンソール表示用 */
uint16_t mode_next_move_s(void);

#endif /* MODE_H */
