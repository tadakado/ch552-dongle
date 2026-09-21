/* SPDX-License-Identifier: MIT */
/*
 * usb_desc.h - USB 記述子 (CDC-ACM + HID の複合デバイス)
 *
 * インタフェースの並び:
 *   IAD          IF0 と IF1 を CDC としてまとめる
 *   IF0          CDC Communications (ACM)      EP3 IN  通知
 *   IF1          CDC Data                      EP2 IN/OUT
 *   IF2          HID (キーボード + マウス)     EP1 IN
 *
 * CDC を複合デバイスに入れるときは IAD (Interface Association
 * Descriptor) を付け、デバイス記述子のクラスを 0xEF/0x02/0x01 に
 * しておくのが安全。これを省くとホストによっては CDC の制御と
 * データのインタフェースを結び付けられず、シリアルポートが出ない。
 *
 * キーボードとマウスは 1 つの HID インタフェースに Report ID で
 * 多重している。端点と USB RAM を CDC のために空けておくため。
 */
#ifndef USB_DESC_H
#define USB_DESC_H

#include <stdint.h>

#define EP0_SIZE        64
#define EP1_SIZE        16   /* HID。最大レポート長は 9 バイト          */
#define EP2_SIZE        64   /* CDC データ (バルク)                     */
#define EP3_SIZE        8    /* CDC 通知 (割り込み)。今は送っていない   */

/* インタフェース番号 */
#define IF_CDC_CTRL     0
#define IF_CDC_DATA     1
#define IF_HID          2

/* Report ID。レポートの先頭 1 バイトに入る */
#define REPORT_ID_KEYBOARD 1
#define REPORT_ID_MOUSE    2

#define KEYBOARD_REPORT_LEN 8   /* modifier + reserved + keycode x6      */
#define MOUSE_REPORT_LEN    4   /* buttons + X + Y + wheel               */

extern __code const uint8_t dev_desc[];
extern __code const uint8_t cfg_desc[];
extern __code const uint8_t hid_report_desc[];
extern __code const uint8_t str_lang[];
extern __code const uint8_t str_vendor[];
extern __code const uint8_t str_product[];
/*
 * シリアル番号だけは実行時にチップ固有 ID から組み立てるので xdata。
 * usb_desc_init_serial() を usb_init() より前に呼ぶこと。
 */
#define SERIAL_BYTES 5                      /* チップ ID は 40bit */
#define SERIAL_CHARS (SERIAL_BYTES * 2)     /* 16 進なので 1 バイト 2 文字 */
extern __xdata uint8_t str_serial[2 + SERIAL_CHARS * 2];

void usb_desc_init_serial(void);

extern __code const uint8_t dev_desc_len;
extern __code const uint8_t cfg_desc_len;
extern __code const uint8_t hid_report_desc_len;

#endif /* USB_DESC_H */
