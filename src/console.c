/* SPDX-License-Identifier: MIT */
#include "ch552.h"
#include "config.h"
#include "sys.h"
#include "tick.h"
#include "cdc.h"
#include "usb.h"
#include "touch.h"
#include "mode.h"
#include "console.h"
#include "usb_desc.h"
#include "hid.h"

/*
 * iRAM は 256 バイトしかなく、USB とタッチの状態で埋まる。
 * コンソール周りは秒単位の粒度でしか動かないので xdata に置く。
 */
static __xdata uint8_t monitor;      /* 生値の連続表示 */
static __xdata uint8_t jiggle_req;
static __xdata uint32_t next_mon;
static __xdata uint8_t was_open;

static void print_help(void)
{
    cdc_puts("\r\nCH552 dummy HID\r\n"
             "  i  status\r\n"
             "  m  monitor on/off\r\n"
             "  +  threshold +10\r\n"
             "  -  threshold -10\r\n"
             "  s  save threshold to data-flash\r\n"
             "  c  clear saved threshold\r\n"
             "  z  recalibrate baseline (do not touch)\r\n"
             "  t  toggle mode A/B\r\n"
             "  j  test mouse jiggle\r\n"
             "  k  send empty keyboard report\r\n"
             "  b  enter bootloader\r\n"
             "  h  this help\r\n");
}

static void print_status(void)
{
    cdc_puts("mode=");
    if (mode_get() == MODE_WIGGLE) {
        cdc_puts("B(wiggle) next=");
        cdc_put_u16(mode_next_move_s());
        cdc_puts("s ");
    } else {
        cdc_puts("A(idle) ");
    }
    cdc_puts("raw=");
    cdc_put_u16(touch_raw());
    cdc_puts(" base=");
    cdc_put_u16(touch_baseline());
    cdc_puts(" delta=");
    cdc_put_u16(touch_delta());
    cdc_puts(" peak=");
    cdc_put_u16(touch_peak_delta());
    cdc_puts(" noise=");
    cdc_put_u16(touch_noise());
    cdc_puts(" thr=");
    cdc_put_u16(touch_threshold());
    cdc_puts(" src=");
    switch (touch_threshold_source()) {
    case TOUCH_THR_FLASH:   cdc_puts("flash"); break;
    case TOUCH_THR_CONFIG:  cdc_puts("config"); break;
    case TOUCH_THR_UNSAVED: cdc_puts("unsaved"); break;
    default:                cdc_puts("auto"); break;
    }
    cdc_puts(touch_is_down() ? " DOWN" : "");
    cdc_put_nl();

    /*
     * 学習ジェスチャの残り回数。黄が出たときに「値が弱すぎたのか、
     * 上限に達したのか」をここで見分ける。
     */
    cdc_puts("learn=");
    cdc_put_u16(touch_learn_writes());
    cdc_puts("/");
    cdc_put_u16(TOUCH_LEARN_MAX_WRITES);
    cdc_put_nl();

    /*
     * シリアル番号。記述子は UTF-16LE なので 1 文字おきに拾う。
     * 実機で 0000000001 なら、チップ ID が読めずに固定値へ落ちている。
     */
    cdc_puts("led=");
    cdc_put_u16(usb_last_led_report());
    cdc_put_nl();

    cdc_puts("serial=");
    {
        uint8_t i;

        for (i = 0; i < SERIAL_CHARS; i++) {
            cdc_putc((char)str_serial[2 + i * 2]);
        }
    }
    cdc_put_nl();
}

void console_init(void)
{
    monitor = 0;
    jiggle_req = 0;
    was_open = 0;
    next_mon = 0;
}

uint8_t console_take_jiggle(void)
{
    uint8_t v = jiggle_req;

    jiggle_req = 0;
    return v;
}

void console_poll(void)
{
    int16_t c;
    uint8_t open_now = usb_cdc_dtr();

    /* ポートが開かれた直後に一度だけ挨拶する */
    if (open_now && !was_open) {
        print_help();
        print_status();
    }
    if (!open_now && was_open) {
        /* 閉じた瞬間に残っていた出力は捨てる (次の挨拶より先に出る) */
        cdc_tx_drop();
    }
    was_open = open_now;

    if (monitor && open_now) {
        uint32_t now = tick_now();

        if ((int32_t)(now - next_mon) >= 0) {
            next_mon = now + CONSOLE_MONITOR_MS;
            cdc_puts("d=");
            cdc_put_u16(touch_delta());
            cdc_puts(" thr=");
            cdc_put_u16(touch_threshold());
            cdc_put_nl();
        }
    }

    c = cdc_getc();
    if (c < 0) {
        return;
    }

    switch ((char)c) {
    case 'i':
        print_status();
        break;

    case 'm':
        monitor = (uint8_t)!monitor;
        cdc_puts(monitor ? "monitor on\r\n" : "monitor off\r\n");
        break;

    case '+':
        touch_set_threshold((uint16_t)(touch_threshold() + 10));
        print_status();
        break;

    case '-': {
        uint16_t t = touch_threshold();

        /*
         * 引き算が uint16 で回り込むと、丸めた先が上限になってしまう。
         * 下限を下回るぶんは touch_set_threshold() が丸めるので、
         * ここで見るのは回り込みだけでよい。
         */
        touch_set_threshold(t > 10 ? (uint16_t)(t - 10)
                                   : TOUCH_THRESHOLD_MIN);
        print_status();
        break;
    }

    case 'k':
        /*
         * 空のキーボードレポート (修飾キーもキーコードも 0) を 1 回送る。
         * 何も打鍵されないので、どのウィンドウにフォーカスがあっても
         * 安全に「ホストがキーボードとして受け取っているか」を試せる。
         * このデバイスは通常運転でキーを一切送らないので、
         * HID 記述子のキーボード部を確かめる手段がこれしかない。
         */
        if (hid_keyboard_release()) {
            cdc_puts("keyboard report sent\r\n");
        } else {
            cdc_puts("EP1 busy\r\n");
        }
        break;

    case 's':
        /*
         * ここは学習ジェスチャと違って回数を制限しない。
         * 意図して打つ操作で、閾値を詰める作業そのものが数回では
         * 終わらないため (config.h の TOUCH_LEARN_MAX_WRITES の項)。
         */
        switch (touch_save_threshold(touch_threshold())) {
        case TOUCH_SAVE_WROTE: cdc_puts("saved\r\n"); break;
        case TOUCH_SAVE_SAME:  cdc_puts("unchanged (already saved)\r\n");
                               break;
        default:               cdc_puts("save failed\r\n"); break;
        }
        break;

    case 'c':
        cdc_puts(touch_clear_threshold() ? "cleared\r\n" : "clear failed\r\n");
        break;

    case 'z':
        /*
         * 較正は 70ms ほどブロックする。その間 USB の割り込みは
         * 動いたままなので列挙は落ちない。
         */
        touch_recalibrate();
        cdc_puts("recalibrated\r\n");
        print_status();
        break;

    case 't':
        mode_toggle();
        print_status();
        break;

    case 'j':
        jiggle_req = 1;
        cdc_puts("jiggle\r\n");
        break;

    case 'b':
        cdc_puts("entering bootloader\r\n");
        cdc_flush();
        /* 送り切る時間を少し置いてから落とす */
        delay_ms(50);
        sys_jump_bootloader();
        break;

    case 'h':
    case '?':
        print_help();
        break;

    default:
        break;
    }
}
