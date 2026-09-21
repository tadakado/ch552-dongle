/* SPDX-License-Identifier: MIT */
/*
 * hid.h - HID レポートの組み立てと送出
 *
 * キーボードとマウスは 1 つのインタフェースに Report ID で
 * 同居させている。どちらも EP1 IN を共有するので、
 * 前のレポートを送り終える前に次を投げると落ちる。
 * 送れたかどうかは戻り値で必ず確認すること。
 */
#ifndef HID_H
#define HID_H

#include <stdint.h>

/* マウスボタン */
#define MOUSE_BTN_LEFT   0x01
#define MOUSE_BTN_RIGHT  0x02
#define MOUSE_BTN_MIDDLE 0x04

/*
 * 相対移動を送る。送信キューに乗ったら 1、
 * 未列挙または前のレポートが未完了なら 0 を返す。
 */
uint8_t hid_mouse_move(int8_t dx, int8_t dy, int8_t wheel, uint8_t buttons);

/*
 * キーボードレポートを送る。keys は 6 バイト (使わない位置は 0)。
 * NULL を渡すと全キー離しになる。
 */
uint8_t hid_keyboard_report(uint8_t modifier, const uint8_t *keys);

/* 全キー離し。押しっぱなしを残さないための後始末 */
uint8_t hid_keyboard_release(void);

#endif /* HID_H */
