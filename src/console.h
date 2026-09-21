/* SPDX-License-Identifier: MIT */
/*
 * console.h - CDC 経由の 1 文字コマンド
 *
 * 閾値を実機だけで詰めるための最後の手段がタッチのジェスチャ、
 * 数値で追い込みたいときがこちら。ターミナルを開いて 1 文字打つだけ。
 *   screen /dev/cu.usbmodem* 115200
 */
#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>

void console_init(void);
void console_poll(void);

/* 動作確認のジグル要求を 1 回だけ拾う */
uint8_t console_take_jiggle(void);

#endif /* CONSOLE_H */
