/* SPDX-License-Identifier: MIT */
#include "ch552.h"
#include "config.h"
#include "usb.h"
#include "usb_desc.h"
#include "hid.h"

uint8_t hid_mouse_move(int8_t dx, int8_t dy, int8_t wheel, uint8_t buttons)
{
    uint8_t *buf;

    if (!usb_ep1_ready()) {
        return 0;
    }
    buf = usb_ep1_tx_buf();
    buf[0] = REPORT_ID_MOUSE;
    buf[1] = buttons;
    buf[2] = (uint8_t)dx;
    buf[3] = (uint8_t)dy;
    buf[4] = (uint8_t)wheel;
    usb_ep1_commit(1 + MOUSE_REPORT_LEN);
    return 1;
}

uint8_t hid_keyboard_report(uint8_t modifier, const uint8_t *keys)
{
    uint8_t *buf;
    uint8_t i;

    if (!usb_ep1_ready()) {
        return 0;
    }
    buf = usb_ep1_tx_buf();
    buf[0] = REPORT_ID_KEYBOARD;
    buf[1] = modifier;
    buf[2] = 0x00; /* 予約バイト */
    for (i = 0; i < 6; i++) {
        buf[3 + i] = keys ? keys[i] : 0x00;
    }
    usb_ep1_commit(1 + KEYBOARD_REPORT_LEN);
    return 1;
}

uint8_t hid_keyboard_release(void)
{
    return hid_keyboard_report(0x00, 0);
}
