/* SPDX-License-Identifier: MIT */
#include "ch552.h"
#include "config.h"
#include "tick.h"
#include "hid.h"
#include "usb.h"
#include "dflash.h"
#include "mode.h"

/* Data-Flash 上の保存場所。タッチの閾値 (0x00-0x05) とは重ならない */
#define MODE_DF_MAGIC 0x07
#define MODE_DF_VALUE 0x08
#define MODE_DF_MAGIC_V 'M'

static __xdata mode_t   cur_mode;
static __xdata uint32_t next_move;
static __xdata int8_t   dir;
static __xdata uint8_t  pending;

static void schedule_next(uint32_t from)
{
    next_move = from + MODE_B_INTERVAL_MS;
}

void mode_init(void)
{
    cur_mode = MODE_DEFAULT;

#if MODE_PERSIST
    /*
     * 記録があれば、それが IDLE でも WIGGLE でも従う。
     * 「WIGGLE のときだけ上書きする」と書くと、MODE_DEFAULT を
     * WIGGLE にしたときに保存した IDLE が復元されない。
     */
    if (dflash_read(MODE_DF_MAGIC) == MODE_DF_MAGIC_V) {
        cur_mode = (dflash_read(MODE_DF_VALUE) == (uint8_t)MODE_WIGGLE)
                       ? MODE_WIGGLE : MODE_IDLE;
    }
#endif

    dir = 1;
    pending = 0;
    schedule_next(tick_now());
}

void mode_toggle(void)
{
    cur_mode = (cur_mode == MODE_IDLE) ? MODE_WIGGLE : MODE_IDLE;

    /*
     * 切り替えた瞬間から 1 分を数え直す。押した直後にいきなり
     * 動かれると、意図した操作なのか誤動作なのか分からなくなる。
     */
    pending = 0;
    dir = 1;
    schedule_next(tick_now());

#if MODE_PERSIST
    /*
     * Data-Flash は約 1 万回で寿命が来る。読み返して同じなら書かない。
     * (以前はコメントだけそう書いてあって、実際は毎回 2 バイト書いて
     *  いた。短押しのたびに 1 回消費するので、1 日 30 回で 1 年もたない。)
     * 書けたかどうかも見る。黙って失敗すると、次の起動で勝手に
     * モードが戻っていて理由が分からない、という形で出る。
     */
    if (dflash_read(MODE_DF_VALUE) != (uint8_t)cur_mode) {
        if (!dflash_write(MODE_DF_VALUE, (uint8_t)cur_mode)) {
            /*
             * 書けなかったら記録ごと無効にする。マジックを残して
             * 帰ると、次の起動で **古い方の値** が復元される。
             * モード B のまま焼き付いた石は、挿しただけでマウスが
             * 動き出す = 一番驚く壊れ方になる。IDLE に落ちる方が安全。
             *
             * この書き込みまで失敗したら打つ手は無い (Data-Flash が
             * 丸ごと死んでいる)。戻り値を見ても何もできないので捨てる。
             * その場合は古いモードで起動する。
             */
            (void)dflash_write(MODE_DF_MAGIC, 0xFF);
            return;
        }
    }
    if (dflash_read(MODE_DF_MAGIC) != MODE_DF_MAGIC_V) {
        (void)dflash_write(MODE_DF_MAGIC, MODE_DF_MAGIC_V);
    }
#endif
}

mode_t mode_get(void) { return cur_mode; }

void mode_poll(void)
{
    uint32_t now;

    if (cur_mode != MODE_WIGGLE) {
        return;
    }

    now = tick_now();

    if (!pending && (int32_t)(now - next_move) >= 0) {
        pending = 1;
    }

    if (pending) {
        /*
         * hid_mouse_move() は未列挙や EP1 が塞がっていると 0 を返す。
         * 送れるまで pending を立てたままにしておくので、
         * ホストがサスペンドから戻った直後に確実に 1 回動く。
         */
        if (hid_mouse_move(dir, 0, 0, 0)) {
            pending = 0;
            dir = (int8_t)-dir;
            schedule_next(now);
        }
    }
}

uint16_t mode_next_move_s(void)
{
    uint32_t now;
    uint32_t left;

    if (cur_mode != MODE_WIGGLE) {
        return 0;
    }
    now = tick_now();
    if ((int32_t)(now - next_move) >= 0) {
        return 0;
    }
    left = next_move - now;
    return (uint16_t)(left / 1000UL);
}

uint8_t mode_color_r(void)
{
    return (cur_mode == MODE_WIGGLE) ? COLOR_WIGGLE_R : COLOR_IDLE_R;
}

uint8_t mode_color_g(void)
{
    return (cur_mode == MODE_WIGGLE) ? COLOR_WIGGLE_G : COLOR_IDLE_G;
}

uint8_t mode_color_b(void)
{
    return (cur_mode == MODE_WIGGLE) ? COLOR_WIGGLE_B : COLOR_IDLE_B;
}
