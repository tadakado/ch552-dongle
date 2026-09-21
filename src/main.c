/* SPDX-License-Identifier: MIT */
/*
 * main.c - CH552 USB HID ダミーデバイス
 *
 * モード:
 *   A IDLE    何もしない。USB HID として繋がっているだけ。LED 緑
 *   B WIGGLE  1 分ごとにマウスを右へ 1、さらに 1 分後に左へ 1。LED 赤
 *
 * タッチの短押しで A <-> B を切り替える。
 *
 * LED の見方 (上にあるものが下を上書きする):
 *   消灯          ホストがサスペンド中。**これが最優先**。サスペンドの
 *                 判定はループの先頭にあり、以下の表示には届かない。
 *                 触れている最中に眠られると青も白も消える
 *   白 1 回        サスペンド中にタッチしてホストを起こそうとした
 *                 (SUSPEND_POLICY = SUSPEND_WAKE のときだけ)
 *   白の点滅      サスペンド中の長押し 3 秒成立
 *                 (SUSPEND_POLICY = SUSPEND_WAKE のときだけ)
 *   --- ここから下はホストが起きているときの表示 ---
 *   マゼンタ点滅  閾値の学習に成功して Data-Flash に保存した
 *                 12.5Hz を 600ms (8 回点滅)
 *   黄の点滅      学習を試みたが採用できなかった (値が妥当でない、
 *                 または Data-Flash に焼けなかった)
 *                 12.5Hz を 400ms (5 回点滅)
 *   白の点滅      長押し 3 秒成立。6.2Hz を 600ms (4 回点滅) して
 *                 消灯し、ブートローダへ飛ぶ
 *   青のべた点灯  タッチ検出中 (3 秒未満)
 *   橙の呼吸      まだ列挙されていない (SET_CONFIGURATION 未達)
 *   モード色の呼吸 列挙済み。モード A = 緑、モード B = 赤。
 *                 指を近づけると呼吸を突き抜けて明るくなる
 *
 * 呼吸は 2.56 秒で一往復。ガンマ補正の下端が 0 なので、1 周期あたり
 * 500ms ほどは完全に消灯する。最大輝度は 0x40。
 *
 * 閾値を変える方法は 3 つある。
 *   1. 実機だけで完結: 1.0〜2.5 秒のあいだ触れて離す。その押下で出た
 *      delta の最大値の半分が新しい閾値になり、Data-Flash に焼かれる。
 *   2. config.h の TOUCH_THRESHOLD_DEFAULT を 0 以外にして焼き直す。
 *   3. CDC のコンソールから + / - で動かして s で保存する。
 * Data-Flash の値は config.h より優先される。
 */
#include "ch552.h"
#include "config.h"
#include "sys.h"
#include "tick.h"
#include "neo.h"
#include "touch.h"
#include "usb.h"
#include "hid.h"
#include "cdc.h"
#include "console.h"
#include "mode.h"
#include "breathe_lut.h"

#define BREATHE_STEP_MS (BREATHE_MS / BREATHE_STEPS)

/* コンソールの j で走らせる動作確認用のジグル */
#define TEST_JIGGLE_GAP_MS 300

enum {
    JIG_IDLE = 0,
    JIG_SEND_RIGHT,
    JIG_WAIT,
    JIG_SEND_LEFT
};

/*
 * 近接量を LED の明るさに写す。
 * delta が閾値に対してどこまで来ているかを 0..NEO_MAX で返す。
 */
static uint8_t proximity_level(void)
{
    uint16_t d = touch_delta();
    uint16_t thr = touch_threshold();

    if (d == 0 || thr == 0) {
        return 0;
    }
    if (d >= thr) {
        return NEO_MAX;
    }
    return (uint8_t)((uint32_t)d * NEO_MAX / thr);
}

void main(void)
{
    uint32_t next_frame = 0;
    uint32_t jig_at = 0;
    uint8_t phase = 0;
    uint8_t frames_in_step = 0;
    uint8_t blink = 0;
#if TOUCH_LEARN_ENABLE
    uint8_t flash_frames = 0;
    uint8_t flash_ok = 0;
#endif
    uint8_t jig_state = JIG_IDLE;
    uint8_t was_suspended = 0;
#if SUSPEND_POLICY == SUSPEND_WAKE
    uint8_t swallow_release = 0;
#endif
    uint32_t suspend_at = 0;
    uint16_t last_hold_ms = 0;

    const uint8_t frames_per_step =
        (BREATHE_STEP_MS + NEO_FRAME_MS - 1) / NEO_FRAME_MS;

    sys_init();
    neo_init();
    tick_init();
    EA = 1;

    /*
     * タッチの較正は指を触れていない前提。72ms (やり直しが入ると最悪
     * 216ms、測定系が黙っていると 576ms) ブロックするので、USB を
     * 上げる前に済ませておく。列挙が始まってから止まるより挿す前に
     * 終わっている方が行儀が良い。この時点ではまだ D+ のプルアップを
     * 上げていないので、ホストからはデバイスが見えていない。
     */
#if TOUCH_ENABLE
    touch_init();
#endif
    mode_init();
    console_init();
    usb_init();

    /*
     * 餌やりが要る処理はここから。ブロックの長い初期化 (touch_init の
     * 72〜576ms) を済ませてから有効にする。順序を逆にすると、
     * タッチが黙っている個体で列挙前にリセットを繰り返すことになり、
     * 1200baud も長押しも届かない = ブートローダへの入口が
     * ハードウェアのパッドだけになる。
     */
    sys_watchdog_init();

    for (;;) {
        uint32_t now;

        sys_watchdog_feed();

        /* --- ホストがサスペンドしている間 --------------------------- */
        /*
         * 列挙済みかどうかは見ない。USB の 500uA 制限はサスペンド中の
         * デバイス全部にかかるもので、未構成なら免除されるわけではない。
         * 眠っている PC の給電ポートに挿したまま放置すると、
         * その条件を入れていると 24MHz で回り続けて電池を舐める。
         *
         * バスに活動がある限り suspended は立たないので、列挙の途中で
         * 眠ることはない。橙の呼吸 (未列挙の表示) もその間は出続ける。
         */
        if (usb_is_suspended()) {
            if (!was_suspended) {
                /*
                 * 消灯は 1 回だけ送れば足りるが、**直前のフレームとの
                 * 間隔が保証されていない**。サスペンドが立つのは
                 * 20ms のフレーム周期のどこでもよく、直前の描画から
                 * 280us 以内だと、この 24bit は「2 個目の LED ぶり」と
                 * 見なされて DOUT に素通りする。1 灯構成では何も起きず、
                 * 直前の呼吸のフレームが点いたままホストのスリープ中
                 * ずっと光り続ける (サスペンド 70 回に 1 回程度)。
                 * 500uA の話をしておいて光らせたままでは意味がない。
                 * NEO_SUSPEND_LEVEL は既定 0 なので真っ暗になる。
                 */
                neo_latch();
                neo_show(mode_color_r(), mode_color_g(), mode_color_b(),
                         NEO_SUSPEND_LEVEL);
                was_suspended = 1;
                suspend_at = tick_now();
                /*
                 * 通常運転中は押下イベントを誰も consume しないので、
                 * 最後に触ったぶんが溜まったままここに来る。掃除して
                 * おかないと、指を離してからホストが眠った直後に
                 * 「古い押下」でリモートウェイクアップを撃ってしまい、
                 * 触ってもいないのに Mac が起きる。離しも同様に捨てる。
                 */
#if TOUCH_ENABLE
                touch_clear_events();
#endif
                last_hold_ms = 0;
#if SUSPEND_POLICY == SUSPEND_WAKE
                /*
                 * この先の長押し予告も同じフレーム変数で間引いている。
                 * 期限を過ぎたまま持ち越すと、指を置いたまま眠られた
                 * 場合に予告の 1 フレーム目が消灯の直後に出てしまい、
                 * これも 280us に届かず素通りする。1 周期ぶん空ける。
                 */
                next_frame = suspend_at + NEO_FRAME_MS;
#endif
            }

#if SUSPEND_POLICY == SUSPEND_SLEEP
            /*
             * USB イベントが来るまで眠る。眠っている間はタッチもタイマも
             * 止まるので、ここから先を回しても意味がない。
             *
             * 判定ごと割り込み禁止の中でやる。上の usb_is_suspended() は
             * 割り込み有効のまま読んでいるので、そこから sys_sleep() に
             * 入るまでの間にレジュームが来る余地がある。ホストが起きて
             * いるのにこちらだけ眠る、という状態になりうる (実際には
             * レジューム後の SOF で起きるので自己修復するが、隙間は
             * 塞いでおく)。
             */
            EA = 0;
            if (usb_is_suspended()) {
                sys_sleep();
            }
            EA = 1;
#else
            /*
             * 起きたまま待ち、タッチされたらホストを起こす。
             * ホストが SET_FEATURE(DEVICE_REMOTE_WAKEUP) で許可して
             * いなければ usb_remote_wakeup() は何もしない。
             */
#if TOUCH_ENABLE
            touch_poll();
            /*
             * ガードを先に見ること。&& を逆に書くと、時間の条件を
             * 満たさない場合でも touch_take_press() が先に走って
             * 押下イベントを食べてしまう。すると swallow_release が
             * 立たないまま離しだけが残り、復帰後の最初のループで
             * held = 0 の短押しと見なされてモードが切り替わる。
             * 「起こそうとしただけなのにモード B に入っていた」になる。
             */
            if ((int32_t)(tick_now() - suspend_at) >= WAKEUP_GUARD_MS &&
                touch_take_press()) {
                if (usb_remote_wakeup()) {
                    /*
                     * 起こしにいったことが分かるように 1 回光らせる。
                     * ホストが起きなかった場合、これだけが手がかりになる。
                     */
                    neo_show(COLOR_DFU_R, COLOR_DFU_G, COLOR_DFU_B, NEO_MAX);
                    delay_ms(80);
                    neo_show(0, 0, 0, 0);
                    neo_latch();
                }
                /*
                 * 起こすために触ったぶんでモードまで切り替わると
                 * 驚くので、この押下に対応する離しは食べてしまう。
                 * 長押しの DFU は保持時間で判定しているので影響しない。
                 */
                swallow_release = 1;
            }

            /*
             * サスペンド中も長押し DFU を効かせる。README の比較表が
             * SUSPEND_WAKE では「長押し DFU が効く」と書いている以上、
             * ここで continue して判定に届かないのは食い違い。
             * ホストが起床を許可していない場合、眠ったままの Mac から
             * 書き直す唯一の入口がこれになる。
             */
            if (touch_is_down()) {
                uint16_t held = touch_hold_ms();

                if (held >= TOUCH_LONGPRESS_MS) {
                    /*
                     * 通常ループと同じく 20ms に間引く。毎周回 (数十 us)
                     * 送ると連続フレームの間に 280us の低区間が入らず、
                     * WS2812 がラッチしないので予告の白点滅が見えない。
                     * さらに 33us の EA=0 と P1 の高速トグルを回し続けて
                     * タッチ測定にノイズを乗せることになる。
                     */
                    uint32_t now2 = tick_now();

                    if ((int32_t)(now2 - next_frame) >= 0) {
                        next_frame = now2 + NEO_FRAME_MS;
                        blink++;
                        neo_show(COLOR_DFU_R, COLOR_DFU_G, COLOR_DFU_B,
                                 (blink & 0x04) ? NEO_MAX : 0);
                    }
#if TOUCH_LONGPRESS_DFU
                    if (held >= (TOUCH_LONGPRESS_MS + 600)) {
                        neo_latch();
                        neo_show(COLOR_DFU_R, COLOR_DFU_G, COLOR_DFU_B, 0);
                        neo_latch();
                        sys_jump_bootloader(); /* 戻ってこない */
                    }
#endif
                }
            }
#endif /* TOUCH_ENABLE */
#endif /* SUSPEND_POLICY == SUSPEND_WAKE */
            continue;
        }
        if (was_suspended) {
            was_suspended = 0;

            /*
             * 眠っている間はベースライン追従が止まる。復帰したときに
             * 温度や湿度で生値が閾値ぶん下がっていると、指も触れて
             * いないのに押下判定が立つ。押下中はベースラインを
             * 追従させない作りなので自分では戻れず、押しっぱなし救済
             * (30 秒) より先に長押し DFU (3.6 秒) に届いてしまう。
             * つまり **Mac が復帰するたびに勝手にブートローダへ落ちる**。
             *
             * 測り直せば済む。指が乗っていた場合は delta が 0 側に
             * 倒れるだけなので安全側。72ms (やり直しが入ると 216ms)
             * かかる。touch_recalibrate() は中で餌をやるので、
             * 同じ周回でコンソールの z が続いても足りなくならない。
             */
#if TOUCH_ENABLE
            touch_recalibrate();
            touch_clear_events();
#endif
            last_hold_ms = 0;
#if SUSPEND_POLICY == SUSPEND_WAKE && TOUCH_ENABLE
            /*
             * 起こすために触ったぶんの離しを食べる旗も下ろす。
             * 残したままだと、復帰後の最初の短押しが食べられて
             * モードが切り替わらない。押下側は touch_clear_events()
             * で消しているので、対応する離しはもう来ない。
             */
            swallow_release = 0;
#endif

            /* 復帰したらすぐ描き直す */
            next_frame = tick_now();
        }

#if TOUCH_ENABLE
        touch_poll();
#endif
        console_poll();
        cdc_flush();

        /*
         * 1200 baud で開いて閉じる合図。割り込みの中では飛べないので
         * ここで拾う。ホストから見ると、ポートを閉じた直後に
         * デバイスが消えて ISP デバイスとして戻ってくる。
         */
        if (usb_dfu_requested()) {
            /*
             * ここもフレーム周期の外なので、前後にラッチを置く。
             * 前を省くと消灯が素通りして、モード色のまま ISP に入る。
             */
            neo_latch();
            neo_show(COLOR_DFU_R, COLOR_DFU_G, COLOR_DFU_B, 0);
            neo_latch();
            sys_jump_bootloader(); /* 戻ってこない */
        }

        if (console_take_jiggle()) {
            jig_state = JIG_SEND_RIGHT;
        }

        /* モード B の 1 分ごとの往復 */
        mode_poll();

        /* --- タッチのジェスチャ処理 -------------------------------- */
#if TOUCH_ENABLE
        if (touch_take_release()) {
            uint16_t held = last_hold_ms;

            /*
             * 使ったら必ず捨てる。残しておくと、指を置いたまま
             * ホストがサスペンドしてスリープに入り、指を離した後で
             * 復帰したときに、古い保持時間で離しが成立する。
             * 1.5 秒が残っていれば復帰のたびに閾値学習が走り、
             * せっかく合わせた保存値を上書きしてしまう。
             * 押しっぱなし救済 (touch.c) も押下無しで離しを立てるので、
             * そちらの経路でも同じことが起きる。
             */
            last_hold_ms = 0;

#if SUSPEND_POLICY == SUSPEND_WAKE && TOUCH_ENABLE
            if (swallow_release) {
                /* ホストを起こすために触ったぶん。何もしない */
                swallow_release = 0;
            } else
#endif
            if (held < TOUCH_SHORT_MAX_MS) {
                /* 短押し (青のうちに離した): モード切替 */
                mode_toggle();
            }
#if TOUCH_LEARN_ENABLE
            else if (held < TOUCH_LONGPRESS_MS) {
                /*
                 * 学習窓 (シアンのうちに離した)。上限が長押しの
                 * 敷居そのものなので、シアンで離せば必ずここに入る。
                 * 白が出る前に離せたなら学習できている、と手元だけで
                 * 判断できるのはこの一致があるため。
                 *
                 * 候補を作る -> 焼けたら採用、の順にする。
                 * touch_save_threshold() は成功したときだけ動作中の
                 * 閾値を差し替えるので、flash_ok がそのまま
                 * 「採用できたか」になり、黄 = 不採用 が嘘にならない。
                 */
                uint16_t thr = touch_peak_threshold(TOUCH_LEARN_DIVISOR);

                /*
                 * touch_learn_threshold() は 0 以外を返したときだけ
                 * その値が有効になっている。既に同じ値が入っていて
                 * 焼かなかった場合 (TOUCH_SAVE_SAME) も 0 以外なので、
                 * マゼンタで正しい。上限に達していた場合は 0 が返り、
                 * 黄になる。
                 */
                flash_ok = (thr != 0)
                             ? (touch_learn_threshold(thr) != TOUCH_SAVE_FAIL)
                             : 0;
                flash_frames = flash_ok ? 30 : 20; /* 600ms / 400ms */
            }
#endif
        }
        if (touch_is_down()) {
            last_hold_ms = touch_hold_ms();
        }
#endif /* TOUCH_ENABLE */

        now = tick_now();

        /* --- 動作確認のジグル -------------------------------------- */
        /*
         * hid_mouse_move() は EP1 が空いていないと 0 を返す。
         * 送れるまで状態を進めないので、取りこぼしは起きない。
         */
        switch (jig_state) {
        case JIG_SEND_RIGHT:
            if (hid_mouse_move(1, 0, 0, 0)) {
                jig_at = now + TEST_JIGGLE_GAP_MS;
                jig_state = JIG_WAIT;
            }
            break;
        case JIG_WAIT:
            if ((int32_t)(now - jig_at) >= 0) {
                jig_state = JIG_SEND_LEFT;
            }
            break;
        case JIG_SEND_LEFT:
            if (hid_mouse_move(-1, 0, 0, 0)) {
                jig_state = JIG_IDLE;
            }
            break;
        default:
            break;
        }

        /* --- LED ---------------------------------------------------- */
        if ((int32_t)(now - next_frame) < 0) {
            continue;
        }
        next_frame = now + NEO_FRAME_MS;
        blink++;

#if TOUCH_LEARN_ENABLE
        if (flash_frames != 0) {
            flash_frames--;
            if (flash_ok) {
                neo_show(COLOR_LEARN_R, COLOR_LEARN_G, COLOR_LEARN_B,
                         (blink & 0x02) ? NEO_MAX : 0);
            } else {
                /*
                 * 失敗は黄で点滅させる。以前は赤のべた点灯だったが、
                 * モード B の赤い呼吸のなかで 200ms だけ全点灯しても
                 * ほとんど見分けがつかなかった。点滅させておけば
                 * どのモードの最中でも「何か起きた」と分かる。
                 */
                neo_show(COLOR_FAIL_R, COLOR_FAIL_G, COLOR_FAIL_B,
                         (blink & 0x02) ? NEO_MAX : 0);
            }
        } else
#endif
#if TOUCH_ENABLE
        if (touch_is_down()) {
            uint16_t held = touch_hold_ms();

            if (held >= TOUCH_LONGPRESS_MS) {
                neo_show(COLOR_DFU_R, COLOR_DFU_G, COLOR_DFU_B,
                         (blink & 0x04) ? NEO_MAX : 0);
#if TOUCH_LONGPRESS_DFU
                if (held >= (TOUCH_LONGPRESS_MS + 600)) {
                    /*
                     * 直前の予告点滅と連続で送ることになるので、
                     * あいだにリセット時間を入れる。入れないと
                     * 2 回目の 24bit が「2 個目の LED ぶん」と
                     * 見なされて素通りし、消灯が反映されないまま
                     * ブートローダに入る。
                     */
                    neo_latch();
                    neo_show(COLOR_DFU_R, COLOR_DFU_G, COLOR_DFU_B, 0);
                    neo_latch();
                    sys_jump_bootloader(); /* 戻ってこない */
                }
#endif
            }
#if TOUCH_LEARN_ENABLE
            else if (held >= TOUCH_LEARN_MIN_MS) {
                /*
                 * 学習窓に入った合図。ここで離せば閾値を学習する。
                 * 窓の出口 (白点滅) と対にして見せるためのもので、
                 * TOUCH_DEBUG_LED では消さない。押している最中の色と
                 * 離したときの結果が対応していないと、白を見る前に
                 * 離せたかどうかでしか成否を判断できなくなる。
                 */
                neo_show(COLOR_LEARNWIN_R, COLOR_LEARNWIN_G,
                         COLOR_LEARNWIN_B, NEO_MAX);
            }
#endif
            else {
#if TOUCH_DEBUG_LED
                /* 触れている間は青。閾値を詰めるときの視認用 */
                neo_show(COLOR_TOUCH_R, COLOR_TOUCH_G, COLOR_TOUCH_B, NEO_MAX);
#else
                /* 表示を切ってあるときはモードの色のまま光らせておく */
                neo_show(mode_color_r(), mode_color_g(), mode_color_b(),
                         NEO_MAX);
#endif
            }
        } else
#endif /* TOUCH_ENABLE */
        {
            /*
             * 呼吸の明るさと近接量の大きい方を採る。
             * 列挙前は橙、列挙後はモードの色。挿しただけで状態が目で分かる。
             */
            uint8_t breath = breathe_lut[phase];
#if TOUCH_DEBUG_LED
            uint8_t prox = proximity_level();
            uint8_t level = (prox > breath) ? prox : breath;
#else
            uint8_t level = breath;
#endif

            /*
             * サスペンド中はループの先頭で continue しているので、
             * ここに来る時点でホストは起きている。
             */
            if (usb_is_configured()) {
                neo_show(mode_color_r(), mode_color_g(), mode_color_b(),
                         level);
            } else {
                neo_show(COLOR_NOUSB_R, COLOR_NOUSB_G, COLOR_NOUSB_B, level);
            }
        }

        if (++frames_in_step >= frames_per_step) {
            frames_in_step = 0;
            phase = (uint8_t)((phase + 1) & (BREATHE_STEPS - 1));
        }
    }
}
