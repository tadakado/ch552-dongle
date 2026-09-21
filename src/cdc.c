/* SPDX-License-Identifier: MIT */
#include "ch552.h"
#include "config.h"
#include "usb.h"
#include "usb_desc.h"
#include "cdc.h"
#include "sys.h"
#include "tick.h"

/*
 * リングバッファ。送信は「メインが書いてメインが吐く」ので競合しないが、
 * 受信は割り込みが書いてメインが読む。head と tail をそれぞれ片方の
 * 文脈からしか進めない作りにしてあるので、8bit 単位の更新で足りる。
 */
#define TX_SIZE 256
/*
 * 受信リングは 1 パケット (EP2_SIZE = 64) を丸ごと受け取れる必要がある。
 * 足りないと、ホストが 64 バイトのバルクを 1 個投げてきたときに
 * 入り切らないぶんを捨てることになる。2 のべき乗にしておくと
 * 剰余がビットマスクに落ちる。
 */
#define RX_SIZE 128

/*
 * 送信リングはヘルプ表示 (300 バイト強) が一度に入る大きさが要る。
 * 足りないと文字列が途中で切れて、化けた出力だけが届く。
 */
static __xdata uint8_t tx_buf[TX_SIZE];
static __xdata uint8_t rx_buf[RX_SIZE];
/*
 * TX_SIZE が 256 なので uint8_t でちょうど一周する。
 * (以前は uint16_t + % 256 にしていたが、値域が 0..255 で
 *  同じである以上まったくの等価で、無駄に 2 バイト使っていた。)
 */
static __xdata uint8_t tx_head, tx_tail;
static volatile __xdata uint8_t rx_head;
static __xdata uint8_t rx_tail;

/* リングが埋まって EP2 OUT を NAK にしている間だけ 1 */
static volatile __xdata uint8_t rx_paused;

/* 割り込みが立て、メインが受信リングを捨てて下ろす */
static volatile __xdata uint8_t rx_discard;

/*
 * 直前に送ったパケットがちょうど EP2_SIZE だったら 1。
 * 次に吐くものが無ければ長さ 0 のパケットを 1 つ送って転送を閉じる。
 */
static __xdata uint8_t tx_need_zlp;

/*
 * ホストがポートを開いたまま読まなくなったときに立てる。
 * 立っている間は満杯待ちをせず即座に捨てるので、
 * 「1 文字ごとに待つ x 残り全部」で秒単位止まることがない。
 * 実際に吐き出せたら (cdc_flush) 自動的に降りる。
 */
/* cdc_reset() が ISR から落とすので volatile が要る */
static volatile __xdata uint8_t tx_stalled;

/* 満杯待ちの期限。20ms なので 16bit で足りる (一周 65.5 秒) */
static __xdata uint16_t tx_deadline;

/* リングの空き。1 枠は満杯と空の区別のために犠牲にする */
static uint8_t rx_free(void)
{
    return (uint8_t)((rx_tail - rx_head - 1) & (RX_SIZE - 1));
}

/*
 * 割り込み文脈から呼ばれる。ここに来る時点で、前回 rx_free() が
 * EP2_SIZE 以上あることを確認して ACK にしてあるので、
 * 1 パケットぶんは必ず入る。
 */
/*
 * 端点が初期化されたときに、割り込みの中から呼ばれる。
 *
 * ここでリングの添字を両方ゼロにしてはいけない。cdc_getc() は
 * 「空でないことを確かめてから rx_tail を進める」という手順で動くので、
 * その間に head と tail をまとめて動かされると、空のリングを
 * 「127 バイト残っている」と誤認して古い入力を読み直す。
 * 前に打った 's' や 't'、悪くすると 'b' (ブートローダ) が
 * 列挙し直した瞬間に再実行される。
 *
 * なので ここでは 1 バイトの旗を立てるだけにして、実際の破棄は
 * rx_tail を所有しているメイン側 (cdc_getc) にやらせる。
 * 1 バイトの読み書きは 8051 では分割されないので競合しない。
 */
void cdc_reset(void)
{
    rx_paused = 0;
    tx_stalled = 0;
    rx_discard = 1;
}

/*
 * 受信を再開してよいかどうかで EP2 OUT の応答を決め直す。
 * バスリセットや CLEAR_FEATURE(ENDPOINT_HALT) で端点を ACK に
 * 戻したときに呼ぶ。空きを見ずに ACK にすると、63 バイトしか
 * 空いていないところに 64 バイト受け取って 1 バイト落としながら
 * ホストには成功を返す、ということが起きる。
 */
/*
 * ポートが閉じられたときに呼ぶ。cdc_flush() は DTR が落ちていると
 * 送らないので、閉じた瞬間に残っていた出力 (monitor の行など、最大 255
 * バイト) が、次に開いたときに挨拶より先に吐き出される。
 * tx_head も tx_tail もメインの持ち物なので、ここで捨ててよい。
 */
void cdc_tx_drop(void)
{
    tx_tail = tx_head;
    tx_need_zlp = 0;
    tx_stalled = 0;
}

void cdc_rx_rearm(void)
{
    if (rx_free() >= EP2_SIZE) {
        rx_paused = 0;
        usb_ep2_rx_set(1);
    } else {
        rx_paused = 1;
        usb_ep2_rx_set(0);
    }
}

void cdc_on_rx(__xdata uint8_t *data, uint8_t len)
{
    uint8_t i;
    uint8_t next;

    for (i = 0; i < len; i++) {
        next = (uint8_t)((rx_head + 1) & (RX_SIZE - 1));
        if (next == rx_tail) {
            break; /* 理屈の上では来ない。念のため */
        }
        rx_buf[rx_head] = data[i];
        rx_head = next;
    }

    /*
     * 次の 1 パケットが入らないなら、受け取る前に断る。
     * console_poll() は 1 周に 1 バイトしか読まないので、
     * 貼り付けのようにまとまって来ると普通に詰まる。
     */
    if (rx_free() < EP2_SIZE) {
        usb_ep2_rx_set(0);
        rx_paused = 1;
    }
}

int16_t cdc_getc(void)
{
    uint8_t c;

    /*
     * 端点が初期化されたら、そこまでに溜まっていた入力は捨てる。
     * rx_tail を持っているのはこちらなので、ここで一気に追いつければ
     * 割り込みと競合しない。
     */
    if (rx_discard) {
        rx_discard = 0;
        rx_tail = rx_head;
    }

    /*
     * 1 パケットぶん空いたら受け取りを再開する。
     *
     * **空読みでも必ず通すこと**。以前はここが「1 バイト取り出せた場合」の
     * 後ろにあり、リングが空だと素通りしていた。すると次の並びで固まる:
     *   1. ホストが 64 バイトの OUT を投げる -> 残り 63 バイトしか空かない
     *      ので cdc_on_rx() が NAK にして rx_paused = 1
     *   2. 読み切る前にバスリセット / SET_CONFIGURATION が来て ep2_reset()
     *      -> cdc_reset() が rx_discard = 1、続く cdc_rx_rearm() は
     *      **まだ満杯のリング**を見てもう一度 NAK
     *   3. 次の cdc_getc() が rx_discard でリングを空にする。しかし
     *      空になった直後なので「取り出せた場合」の枝には入らない
     *   4. 以後リングは永久に空、端点は永久に NAK。受信の再開を書く場所は
     *      4 箇所しかなく、そのどれにも届かない
     * 見た目は「再列挙のあと、出力は出るのに一切キーを受け付けない」。
     * 判定を取り出しの前に出しておけば、この状態から必ず抜けられる。
     */
    if (rx_paused && rx_free() >= EP2_SIZE) {
        rx_paused = 0;
        usb_ep2_rx_set(1);
    }

    if (rx_tail == rx_head) {
        return -1;
    }
    c = rx_buf[rx_tail];
    rx_tail = (uint8_t)((rx_tail + 1) & (RX_SIZE - 1));
    return (int16_t)c;
}

void cdc_putc(char c)
{
    uint8_t next = (uint8_t)(tx_head + 1);

    /*
     * 満杯なら少しだけ吐き出しを待つ。ホストが読んでいれば 1ms 程度で
     * 空くので、まとまった出力が途中で切れずに済む。
     *
     * 待ちには 2 つの落とし穴がある。
     *
     * 1. ポートが開いていない (DTR が落ちている) 間は永遠に空かない。
     *    -> DTR を見て即座に捨てる。
     * 2. DTR は立っているのにホストが IN を出さなくなることがある
     *    (読み手が死んだのに別プロセスがポートを掴んだままなど)。
     *    以前はここを「1 文字あたり 5000 周」で打ち切っていたが、
     *    上限が文字単位なので、あふれた残り全部がそれぞれ上限まで
     *    回ることになる。挨拶は 390 バイトでリングは 255 なので
     *    毎回あふれる。結果として 1 回の console_poll() で数秒
     *    止まり、餌やりが main の 1 箇所しかないウォッチドッグ
     *    (約 700ms) が落ちる。挿し直すとまた挨拶するのでリセットが
     *    延々と続く。
     *    -> 待ちは時間で区切り、諦めたら「その burst の残りも捨てる」
     *       という粘着フラグを立てる。待っている間も餌はやる。
     */
    if (next == tx_tail) {
        if (tx_stalled || !usb_cdc_dtr()) {
            return;
        }
        tx_deadline = (uint16_t)(tick_now16() + CDC_TX_WAIT_MS);
        do {
            sys_watchdog_feed();
            cdc_flush();
            if (!usb_cdc_dtr()) {
                return;
            }
            next = (uint8_t)(tx_head + 1);
            if (next != tx_tail) {
                break;
            }
        } while ((int16_t)(tick_now16() - tx_deadline) < 0);

        if (next == tx_tail) {
            tx_stalled = 1;
            return;
        }
    }
    tx_buf[tx_head] = (uint8_t)c;
    tx_head = next;
}

void cdc_puts(__code const char *s)
{
    while (*s) {
        cdc_putc(*s++);
    }
}

void cdc_put_nl(void)
{
    cdc_putc('\r');
    cdc_putc('\n');
}

void cdc_put_u16(uint16_t v)
{
    char digits[5];
    int8_t n = 0;

    if (v == 0) {
        cdc_putc('0');
        return;
    }
    while (v != 0 && n < 5) {
        digits[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n--) {
        cdc_putc(digits[n]);
    }
}

void cdc_flush(void)
{
    uint8_t *dst;
    uint8_t n = 0;

    if (tx_head == tx_tail && !tx_need_zlp) {
        return;
    }
    if (!usb_ep2_ready()) {
        return;
    }
    /*
     * ホストがポートを開いていない (DTR が落ちている) 間は送らない。
     * 送ってしまうと EP2 が塞がったまま戻らず、バッファが無駄に詰まる。
     */
    if (!usb_cdc_dtr()) {
        return;
    }

    dst = usb_ep2_tx_buf();
    while (tx_tail != tx_head && n < EP2_SIZE) {
        dst[n++] = tx_buf[tx_tail];
        tx_tail = (uint8_t)(tx_tail + 1);
    }

    /*
     * バルク転送は「最大パケット長より短いパケット」で終わりを示す。
     * ちょうど 64 バイトで打ち切ると、64 より大きい読み取りを出して
     * いるホスト (Linux の cdc-acm は 128 バイトの URB を使う) は
     * 「まだ続きがある」と思って待ち続け、最後の 64 バイトが
     * 次に何か出力するまで画面に出ない。
     * 長さ 0 のパケットを 1 つ送って閉じる。
     */
    tx_need_zlp = (uint8_t)(n == EP2_SIZE);
    usb_ep2_commit(n);

    /* 吐き出せたということはホストが読んでいる。諦めを解除する */
    tx_stalled = 0;
}
