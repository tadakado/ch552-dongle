/* SPDX-License-Identifier: MIT */
/*
 * usb.h - CH552 USB デバイスコア (制御転送と端点管理)
 *
 * 端点の割り当て:
 *   EP0        制御
 *   EP1 IN     HID レポート
 *   EP2 IN/OUT CDC のデータ
 *   EP3 IN     CDC の通知 (今は何も送っていない)
 */
#ifndef USB_H
#define USB_H

#include <stdint.h>
#include "ch552.h"

void usb_init(void);

/*
 * SDCC は main を含むモジュールから宣言が見えていないと
 * 割り込みベクタを張らないので、ここで宣言しておく。
 */
void usb_isr(void) __interrupt(INT_NO_USB);

/* SET_CONFIGURATION が来て使える状態になったか */
uint8_t usb_is_configured(void);

/* ホストからサスペンドされているか */
uint8_t usb_is_suspended(void);

/* --- EP1 (HID レポート用の割り込み IN) --------------------------- */
uint8_t  usb_ep1_ready(void);
uint8_t *usb_ep1_tx_buf(void);
void     usb_ep1_commit(uint8_t len);

/* --- EP2 (CDC データ用のバルク IN) ------------------------------- */
uint8_t  usb_ep2_ready(void);
uint8_t *usb_ep2_tx_buf(void);

/* EP2 OUT の応答を切り替える。1 = ACK (受け取る), 0 = NAK (待たせる) */
void usb_ep2_rx_set(uint8_t accept);
void     usb_ep2_commit(uint8_t len);

/* --- CDC の状態 --------------------------------------------------- */

/* DTR が立っているか。ホストがポートを開いていればおおむね 1 */
uint8_t usb_cdc_dtr(void);


/*
 * 1200 baud で開いて閉じる合図を受け取ったか。
 * 割り込みの中でブートローダへ飛ぶと制御転送のステータスステージを
 * 返せないので、フラグだけ立ててメインループから飛ぶ。
 */
uint8_t usb_dfu_requested(void);

/* ホストから届いたキーボード LED の状態 (bit0 NumLock, 1 Caps, 2 Scroll) */
uint8_t usb_last_led_report(void);


/* --- リモートウェイクアップ -------------------------------------- */


/*
 * ホストを起こす。サスペンド中かつ許可されているときだけ動き、
 * 実際に信号を出したら 1 を返す。
 */
uint8_t usb_remote_wakeup(void);

#endif /* USB_H */
