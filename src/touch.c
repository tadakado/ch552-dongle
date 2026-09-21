/* SPDX-License-Identifier: MIT */
/*
 * touch.c - CH552 内蔵タッチキードライバ
 *
 * ハードウェアの挙動で押さえておくこと:
 *
 *  - チャネル番号は 1 始まり。TKEY_CTRL の下位 3bit は
 *    0=停止 / 1=TIN0 / 2=TIN1 / 3=TIN2 / 4=TIN3 / 5=TIN4 / 6=TIN5 / 7=未選択。
 *    TIN5 を使うのに 5 を書くと TIN4 を測ってしまう。
 *  - 指が触れると測定値は「下がる」。ベースラインからの下降量で判定する。
 *  - TKEY_DATH の bit7 は bTKD_CHG で、制御を変えた直後は値が無効。
 *    bit6 は予約。データ本体は TKEY_DATH[5:0]:TKEY_DATL の **14bit**
 *    (最大 0x3FFF) なので、上位のマスクは 0x3F。
 *  - 測定周期は 1ms (bTKC_2MS=0)。1 周期は「準備 87us + 検出 913us」で、
 *    検出が終わった時点 (= 周期の終わり) で bTKC_IF が立ち、**次の周期の
 *    準備期間の終わりで**落ちる。TKEY_DAT の方は準備期間のあいだ値を
 *    保持していて、準備期間の終わりで 0 に戻る。
 *    **つまりフラグが立っていて値が読めるのは 1ms のうち 87us だけ**。
 *    「立っているか」だけを見て読むと同じサンプルを何度も拾ってしまうので、
 *    必ず立ち上がりエッジで 1 回だけ取り込む。
 *    裏を返すと、**メインループ 1 周が 87us を超えると高の窓を跨いで
 *    取りこぼしうる** (1ms を超えれば確実に落ちる)。詰まるのではなく
 *    間引かれるだけなので判定が遅れる方向にしか外れないが、
 *    TOUCH_DEBOUNCE も TOUCH_BASE_PERIOD も「サンプル数」で数えている
 *    ので、ループが重くなるとそのぶん実時間が伸びる。
 *  - 入力ピンはハイインピーダンス (P1_DIR_PU=0, P1_MOD_OC=0) でなければならない。
 *
 * ノイズ対策として、WS2812 の送出が同じ P1 ポートで走ることに注意。
 * 30us ほど割り込みを止めて高速にトグルするので、そのタイミングに
 * 当たったサンプルは荒れる。外れ値除去と移動平均で吸収している。
 */
#include "ch552.h"
#include "config.h"
#include "dflash.h"
#include "sys.h"
#include "tick.h"
#include "touch.h"

/* ------------------------------------------------------------------ */
/* Data-Flash 上のパラメータ配置                                       */
/* ------------------------------------------------------------------ */
#define TP_MAGIC0   0x00
#define TP_MAGIC1   0x01
#define TP_VER      0x02
#define TP_THR_L    0x03
#define TP_THR_H    0x04
#define TP_SUM      0x05   /* 0x00..0x04 の総和 (下位 8bit) */

#define TP_MAGIC0_V 'T'
#define TP_MAGIC1_V 'K'
#define TP_VER_V    0x01

/* ------------------------------------------------------------------ */
/* 内部状態                                                            */
/* ------------------------------------------------------------------ */
/*
 * iRAM は 256 バイトしかなく、USB とタッチの状態でほぼ埋まる。
 * SDCC がローカルの一時領域に使うオーバレイ領域まで含めて満杯に
 * すると、__data 変数を 1 つ足しただけでリンクが落ちる。
 * 1kHz でしか触らないもの、コンソールからしか読まないものは
 * xdata に置いて iRAM を空けておく。
 */
static __xdata uint8_t  tk_ctrl;   /* TKEY_CTRL に書く値 (チャネル込み) */
static uint8_t  tk_if_prev;        /* bTKC_IF の前回値 (エッジ検出用)   */
static uint16_t tk_filtered;
/*
 * 32bit は __xdata に置く。iRAM は 256 バイトしかなく、USB とタッチで
 * ほぼ埋まっていて、SDCC がローカルの一時領域に使うオーバレイ領域が
 * 数バイトしか残らない。1kHz でしか触らないので xdata で困らない。
 */
static __xdata uint32_t tk_acc;    /* 移動平均のアキュムレータ          */
static uint16_t tk_baseline;
static __xdata uint16_t tk_threshold;
static __xdata uint16_t tk_noise;
static __xdata touch_thr_src_t tk_src;

/*
 * 学習ジェスチャで焼いた回数。電源 ON からの通算で、挿し直すと戻る。
 * Data-Flash に持たせない (持たせると、回数を数えるためにその
 * Data-Flash を焼くことになって本末転倒)。
 */
static __xdata uint8_t tk_learn_writes;

static uint8_t  tk_down;
static uint8_t  tk_streak;
static uint8_t  tk_press_evt;
static uint8_t  tk_release_evt;
static __xdata uint32_t tk_down_since;
static __xdata uint16_t tk_peak;      /* 押下中に観測した delta の最大値 */
static uint8_t  tk_base_div;  /* ベースライン追従の間引きカウンタ */
static __xdata uint16_t tk_med[3];    /* メディアン用の直近 3 サンプル */

/* ------------------------------------------------------------------ */
/* 低レベル                                                            */
/* ------------------------------------------------------------------ */

/*
 * bTKC_IF の立ち上がりを待って 1 サンプル読む。初期化中しか使わない。
 *
 * 空回りの上限は **時間** で切る。以前は 16bit カウンタが一周したら
 * 抜ける形だったが、内側は 20 周期前後しかないので 1 回あたり 50ms、
 * 較正 1 回ぶん (72 サンプル) では 3〜4 秒になり、ウォッチドッグの
 * 700ms が先に走る。「壊れていても止まらない」つもりの保険が
 * 「壊れているとリセットを繰り返す」に化けていた。
 *
 * 変換周期は 1ms なので、2ms 待って動かなければ測定系が死んでいると
 * 見なしてベースラインを返す。
 *
 * tick は 1ms 刻みで、t0 を取る位置は周期の中のどこでもよい。だから
 * 「差が TK_WAIT_MS を超えた」が成立するまでの実時間は
 * (TK_WAIT_MS, TK_WAIT_MS+1] ms。2 なら 1 回の待ちが最大 3ms、
 * 1 サンプルは待ち 2 回で 6ms、較正 1 回 (72 サンプル) で **432ms**。
 * ウォッチドッグの 700ms を単独では超えない。ここを 4 にすると
 * 720ms になって単独で超えるので、この値は詰めておく意味がある。
 *
 * 空回り回数の方も一応数えておく。tick は Timer2 の割り込みで進むので、
 * EA = 0 で呼ばれると時間の条件が永遠に成立しない。いまの呼び出し元は
 * 全て EA = 1 だが、touch.h から見える関数の前提を「割り込みが動いて
 * いること」だけにしておくと、いつか静かに固まる。
 * この上限は「時間の条件が効かないときだけ働く逃げ道」なので、健全な
 * 系で先に当たらないだけの余裕を取る: 1 周は実測 89 サイクルなので
 * 3ms に相当するのが 24MHz で約 808 回、16MHz で約 540 回。2048 なら
 * 2.5 倍の余裕があり、当たったときは 24MHz で 7.6ms / 16MHz で 11.4ms。
 */
#define TK_WAIT_MS    2
#define TK_WAIT_SPINS 2048U

static uint16_t tk_read_blocking(void)
{
    uint16_t t0;
    uint16_t spins;
    uint8_t hi, lo;

    /* 前のサンプルのフラグが落ちる (=次の変換が始まる) のを待つ */
    t0 = tick_now16();
    spins = 0;
    while (TKEY_CTRL & bTKC_IF) {
        if ((uint16_t)(tick_now16() - t0) > TK_WAIT_MS ||
            ++spins >= TK_WAIT_SPINS) {
            return tk_baseline; /* 壊れていても止まらないようにする */
        }
    }
    /* 変換完了を待つ */
    t0 = tick_now16();
    spins = 0;
    while ((TKEY_CTRL & bTKC_IF) == 0) {
        if ((uint16_t)(tick_now16() - t0) > TK_WAIT_MS ||
            ++spins >= TK_WAIT_SPINS) {
            return tk_baseline;
        }
    }

    lo = TKEY_DATL;
    hi = TKEY_DATH;
    if (hi & bTKD_CHG) {
        return tk_baseline; /* 制御変更直後。この値は信用しない */
    }
    return (uint16_t)(((uint16_t)(hi & 0x3F) << 8) | lo);
}

/* ------------------------------------------------------------------ */
/* Data-Flash パラメータ                                               */
/* ------------------------------------------------------------------ */
static uint8_t tk_load_params(uint16_t *thr)
{
    uint8_t b0 = dflash_read(TP_MAGIC0);
    uint8_t b1 = dflash_read(TP_MAGIC1);
    uint8_t ver = dflash_read(TP_VER);
    uint8_t tl = dflash_read(TP_THR_L);
    uint8_t th = dflash_read(TP_THR_H);
    uint8_t sum = dflash_read(TP_SUM);

    if (b0 != TP_MAGIC0_V || b1 != TP_MAGIC1_V || ver != TP_VER_V) {
        return 0;
    }
    if ((uint8_t)(b0 + b1 + ver + tl + th) != sum) {
        return 0;
    }
    *thr = (uint16_t)(((uint16_t)th << 8) | tl);
    if (*thr == 0 || *thr > TOUCH_THRESHOLD_MAX) {
        return 0;
    }
    return 1;
}

/*
 * thr が既に Data-Flash に入っているか。
 *
 * src まで見るのが肝で、値の一致だけで判断すると、コンソールの
 * + / - で動かした直後 (UNSAVED) や、焼くのに失敗して記録の方が
 * 壊れた状態でも「もう入っている」と答えてしまう。どちらも
 * Data-Flash の中身は thr ではないので、焼かないと次の起動で戻る。
 */
static uint8_t thr_already_saved(uint16_t thr)
{
    return (uint8_t)(thr == tk_threshold && tk_src == TOUCH_THR_FLASH);
}

/*
 * thr を Data-Flash に焼く。**成功したときだけ** 動作中の閾値にも
 * 反映する。順序が逆だと「焼けなかったのに RAM だけ新しい値になって
 * いる」状態が残り、学習失敗の黄色い点滅 (= 採用しなかった) を出しながら
 * 実際には採用している、という食い違いになる。弱く触って学習した低い
 * 閾値がそのまま生きると、ノイズで押されっぱなしになり、そこから
 * 3.6 秒でブートローダに落ちる。抜き差しすると RAM が消えて直るので、
 * 症状だけ見ても原因に辿り着けない。
 */
touch_save_t touch_save_threshold(uint16_t thr)
{
    uint8_t tl, th, sum;

    /*
     * 焼く前に範囲を確かめる。ここを素通しにすると、コンソールで
     * 下げすぎた値がそのまま Data-Flash に入り、起動するたび
     * 誤検出する状態が再現してしまう。
     */
    if (thr < TOUCH_THRESHOLD_MIN || thr > TOUCH_THRESHOLD_MAX) {
        return TOUCH_SAVE_FAIL;
    }

    /*
     * 同じ値なら焼かない。Data-Flash は約 1 万回で寿命が来るので、
     * 中身が変わらない書き込みに 1 回ぶん使う理由が無い。
     * 「マジックを潰す -> 中身 -> マジック」の 7 回書きなので、
     * 素通しにすると 1 回の学習で 7 回ぶん減ることになる。
     */
    if (thr_already_saved(thr)) {
        return TOUCH_SAVE_SAME;
    }

    tl = (uint8_t)(thr & 0xFF);
    th = (uint8_t)(thr >> 8);
    sum = (uint8_t)(TP_MAGIC0_V + TP_MAGIC1_V + TP_VER_V + tl + th);

    /*
     * 先にマジックを潰して「無効」にしてから中身を書き、最後に
     * マジックを立て直す。この順でないと、書き換えの途中で電源が
     * 落ちたときに「古いマジック + 新しい中身」という組み合わせが
     * でき、チェックサムをすり抜ける確率が 1/256 残る。
     *
     * 1 つでも失敗したらそこで止める。書き続けても意味が無いうえ、
     * マジックが潰れたままなので次の起動では自動決定に戻る
     * (壊れた値を読むよりはよほど良い)。
     */
    if (!dflash_write(TP_MAGIC0, 0xFF)) {
        /* まだ何も壊していない。前の記録はそのまま生きている */
        return TOUCH_SAVE_FAIL;
    }
    if (!dflash_write(TP_THR_L, tl) ||
        !dflash_write(TP_THR_H, th) ||
        !dflash_write(TP_VER, TP_VER_V) ||
        !dflash_write(TP_SUM, sum) ||
        !dflash_write(TP_MAGIC1, TP_MAGIC1_V) ||
        !dflash_write(TP_MAGIC0, TP_MAGIC0_V)) {
        /*
         * ここまで来ているならマジックはもう潰れている。動作中の閾値は
         * 変えていないが、**次の起動では自動決定に戻る**。src をそのまま
         * flash にしておくと `i` が「保存済み」と言い続けるので、
         * 食い違っていることが分かる値にしておく。
         */
        if (tk_src == TOUCH_THR_FLASH) {
            tk_src = TOUCH_THR_UNSAVED;
        }
        return TOUCH_SAVE_FAIL;
    }

    tk_threshold = thr;
    tk_src = TOUCH_THR_FLASH;
    return TOUCH_SAVE_WROTE;
}

touch_save_t touch_learn_threshold(uint16_t thr)
{
    touch_save_t r;

    /*
     * 同値の判定を上限より先に置くこと。逆にすると、上限に達した後で
     * 既に入っているのと同じ値を出したときに黄 (不採用) が出る。
     * 実際にはその値が有効なままなので、表示だけが嘘になる。
     */
    if (thr_already_saved(thr)) {
        return TOUCH_SAVE_SAME;
    }
    if (tk_learn_writes >= TOUCH_LEARN_MAX_WRITES) {
        return TOUCH_SAVE_FAIL;
    }

    r = touch_save_threshold(thr);
    if (r == TOUCH_SAVE_WROTE) {
        tk_learn_writes++;
    }
    return r;
}

uint8_t touch_learn_writes(void) { return tk_learn_writes; }

uint8_t touch_clear_threshold(void)
{
    if (!dflash_write(TP_MAGIC0, 0xFF)) {
        return 0;
    }
    /*
     * 記録を消したのだから、動いている値はもう Data-Flash 由来では
     * ない。ここを flash のままにすると 2 つ壊れる。`i` が「保存済み」と
     * 言い続けるのと、thr_already_saved() が「もう入っている」と答えて
     * しまい、消した直後に同じ値を学習しても焼かずに済ませてしまう。
     * どちらも次の起動で自動決定に戻って発覚する。
     */
    if (tk_src == TOUCH_THR_FLASH) {
        tk_src = TOUCH_THR_UNSAVED;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* 初期化                                                              */
/* ------------------------------------------------------------------ */
void touch_recalibrate(void)
{
    uint8_t attempt;

    /*
     * 較正の窓に指の出入りが重なると vmax-vmin が跳ね上がり、
     * そのノイズ幅から起こす自動閾値が桁違いに大きくなる。
     * 「挿すときに端子をかすった」だけで、その電源サイクルのあいだ
     * ずっとタッチが効かなくなるので、明らかにおかしい窓は捨てて
     * 測り直す。それでも収まらなければ最後の値を使う (少なくとも
     * 閾値には TOUCH_THRESHOLD_MAX の上限がある)。
     *
     * **やり直しは丸ごと繰り返す**ので、この関数のブロック時間は
     * 1 回ぶん (8+64 = 72ms) ではなく最悪 3 倍の 216ms。さらに
     * 測定系が黙っていると 1 サンプルあたり TK_WAIT_MS の 2 倍まで
     * 伸びて 576ms に達する。どちらもウォッチドッグの 700ms を
     * 単独では超えないが、同じ周回で 2 回呼ばれる経路 (復帰直後 +
     * コンソールの z) があるので、サンプルごとに餌をやっておく。
     * 上限が構造的に決まっているループなので、ここで餌をやっても
     * ウォッチドッグの意味は損なわれない。
     */
    for (attempt = 0; attempt < TOUCH_CAL_RETRY; attempt++) {
        uint32_t sum = 0;
        uint16_t vmin = 0xFFFF;
        uint16_t vmax = 0;
        uint8_t i;

        for (i = 0; i < TOUCH_DISCARD_SAMPLES; i++) {
            sys_watchdog_feed();
            (void)tk_read_blocking();
        }

        for (i = 0; i < TOUCH_CAL_SAMPLES; i++) {
            uint16_t v;

            sys_watchdog_feed();
            v = tk_read_blocking();
            sum += v;
            if (v < vmin) {
                vmin = v;
            }
            if (v > vmax) {
                vmax = v;
            }
        }

        tk_baseline = (uint16_t)(sum / TOUCH_CAL_SAMPLES);
        tk_noise = (uint16_t)(vmax - vmin);
        if (tk_noise <= TOUCH_NOISE_SANE) {
            break;
        }
    }

    tk_filtered = tk_baseline;
    tk_acc = (uint32_t)tk_baseline << TOUCH_AVG_SHIFT;
    tk_down = 0;
    tk_streak = 0;
    tk_peak = 0;
    tk_base_div = 0;
    tk_med[0] = tk_baseline;
    tk_med[1] = tk_baseline;
    tk_med[2] = tk_baseline;
    tk_down_since = 0;
    tk_if_prev = (TKEY_CTRL & bTKC_IF) ? 1 : 0;
}

void touch_init(void)
{
    uint16_t thr;

    /*
     * タッチ入力はハイインピーダンスでなければならない。
     * WS2812 の P1.1 はプッシュプル出力のままにしたいので、
     * P1 全体ではなく対象ビットだけを落とす。
     */
    P1_DIR_PU &= (uint8_t) ~(1 << TOUCH_PIN_BIT);
    P1_MOD_OC &= (uint8_t) ~(1 << TOUCH_PIN_BIT);

    tk_ctrl = TKEY_CH_CODE(TOUCH_CH); /* 1 始まりなので TIN5 -> 6 */
#if TOUCH_SAMPLE_2MS
    tk_ctrl |= bTKC_2MS;
#endif
    TKEY_CTRL = tk_ctrl;

    touch_recalibrate();

    /* 閾値の決定。優先度は Data-Flash > config.h > 自動 */
    if (tk_load_params(&thr)) {
        tk_threshold = thr;
        tk_src = TOUCH_THR_FLASH;
    } else {
#if TOUCH_THRESHOLD_DEFAULT != 0
        tk_threshold = TOUCH_THRESHOLD_DEFAULT;
        tk_src = TOUCH_THR_CONFIG;
#else
        /*
         * ノイズ幅の定数倍を閾値にする。ノイズが極端に小さい個体で
         * 閾値が下がりすぎないよう下限で押さえる。
         */
        uint32_t t = (uint32_t)tk_noise * TOUCH_NOISE_MULT;
        if (t < TOUCH_THRESHOLD_MIN) {
            t = TOUCH_THRESHOLD_MIN;
        }
        if (t > TOUCH_THRESHOLD_MAX) {
            t = TOUCH_THRESHOLD_MAX;
        }
        tk_threshold = (uint16_t)t;
        tk_src = TOUCH_THR_AUTO;
#endif
    }

    tk_press_evt = 0;
    tk_release_evt = 0;
}

/* ------------------------------------------------------------------ */
/* 毎周回の処理                                                        */
/* ------------------------------------------------------------------ */
void touch_poll(void)
{
    uint8_t if_now = (TKEY_CTRL & bTKC_IF) ? 1 : 0;
    uint8_t hi, lo;
    uint16_t raw;
    uint16_t delta;
    uint16_t thr_off;
    uint32_t now;

    /* 立ち上がりエッジでだけ取り込む。1ms に 1 サンプルになる */
    if (!(if_now && !tk_if_prev)) {
        tk_if_prev = if_now;
        return;
    }
    tk_if_prev = if_now;

    lo = TKEY_DATL;
    hi = TKEY_DATH;
    if (hi & bTKD_CHG) {
        return;
    }
    raw = (uint16_t)(((uint16_t)(hi & 0x3F) << 8) | lo);

    /*
     * WS2812 の送出 (32us) と重なったサンプルは大きく飛ぶ。弾きたいのは
     * その「1 サンプルだけの跳び」なので、3 点のメディアンを通す。
     * 単発の跳びは振幅に関係なく消え、指のような連続した変化は
     * 1ms 遅れるだけで素通りする。
     *
     * 以前は「前回の平滑値からの差が TOUCH_JUMP を超えたら捨てる」に
     * していたが、IIR は生値に遅れてついてくるので、速くて強いタッチ
     * ほど差が開く。delta 2000 を 8ms で置くと差が 1800 になって
     * 丸ごと捨てられ、しかも捨てると平滑値が更新されないので以後も
     * 捨て続け、0.2 秒後に「指が乗った状態」を較正してしまう。
     * 「ゆっくり触ると効くのに、しっかり速く触ると効かない」という、
     * 段階 2 で潰した症状の鏡像になっていた。
     * 閾値も外れ値カウンタも要らなくなるので、まとめて撤去した。
     */
    tk_med[0] = tk_med[1];
    tk_med[1] = tk_med[2];
    tk_med[2] = raw;
    if (tk_med[0] > tk_med[1]) {
        raw = (tk_med[1] > tk_med[2]) ? tk_med[1]
            : ((tk_med[0] > tk_med[2]) ? tk_med[2] : tk_med[0]);
    } else {
        raw = (tk_med[0] > tk_med[2]) ? tk_med[0]
            : ((tk_med[1] > tk_med[2]) ? tk_med[2] : tk_med[1]);
    }

    /* 1/2^TOUCH_AVG_SHIFT の IIR 移動平均 */
    tk_acc = tk_acc - (tk_acc >> TOUCH_AVG_SHIFT) + raw;
    tk_filtered = (uint16_t)(tk_acc >> TOUCH_AVG_SHIFT);

    /* 触れると値は下がる。上に振れた場合は delta = 0 とみなす */
    delta = (tk_baseline > tk_filtered)
                ? (uint16_t)(tk_baseline - tk_filtered)
                : 0;

    /* 離し判定は閾値より低いところに置く (チャタリング防止) */
    thr_off = (uint16_t)(tk_threshold - (tk_threshold >> TOUCH_HYST_SHIFT));
    if (thr_off == 0) {
        thr_off = 1;
    }

    now = tick_now();

    if (!tk_down) {
        if (delta >= tk_threshold) {
            if (++tk_streak >= TOUCH_DEBOUNCE) {
                tk_down = 1;
                tk_streak = 0;
                tk_press_evt = 1;
                tk_down_since = now;
                tk_peak = delta;
            }
        } else {
            tk_streak = 0;
            /*
             * 触れていない間だけベースラインを追従させる。
             * 温度や湿度でじわじわ動くのを吸収するため。
             * 押している間も追従させると、長押しの途中で
             * 勝手に離した判定になってしまう。
             */
            /*
             * 上下とも +1 の下限を付けて対称にしてある。ここを
             * 下降側だけ「step が 0 なら動かさない」にすると、
             * 差が 2^TOUCH_BASE_SHIFT 未満のあいだベースラインが
             * 下に追従できなくなる。生値がゆっくり下がる向きに
             * ドリフトすると差が最大 63 カウントまで溜まり、
             * 自動決定の閾値下限 (40) を超えて誤検出する。
             * しかも触ると値が下がるのだから、追従できない向きが
             * よりによって誤検出する向きになる。
             */
            /*
             * 追従は TOUCH_BASE_PERIOD サンプルに 1 回だけ。
             * 毎サンプル動かすと下限の +1 だけで 1000 カウント/秒に
             * なり、ゆっくり置いた指を丸ごと食べてしまう。
             */
            if (++tk_base_div >= TOUCH_BASE_PERIOD) {
                tk_base_div = 0;
                if (tk_filtered > tk_baseline) {
                    uint16_t d = (uint16_t)(tk_filtered - tk_baseline);
                    tk_baseline += (uint16_t)((d >> TOUCH_BASE_SHIFT) + 1);
                } else if (tk_filtered < tk_baseline) {
                    uint16_t d = (uint16_t)(tk_baseline - tk_filtered);
                    tk_baseline -= (uint16_t)((d >> TOUCH_BASE_SHIFT) + 1);
                }
            }
        }
    } else {
        if (delta <= thr_off) {
            if (++tk_streak >= TOUCH_DEBOUNCE) {
                tk_down = 0;
                tk_streak = 0;
                tk_release_evt = 1;
            }
        } else {
            tk_streak = 0;
        }

        if (delta > tk_peak) {
            tk_peak = delta;
        }

        /*
         * 貼り付き対策。指を触れたまま電源を入れた場合や、
         * 結露などでベースラインがずれたまま固まった場合に、
         * 一定時間で強制的に測り直す。
         */
        if ((uint16_t)(now - tk_down_since) > TOUCH_STUCK_MS) {
            touch_recalibrate();
            tk_release_evt = 1;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 参照系                                                              */
/* ------------------------------------------------------------------ */
uint8_t touch_is_down(void) { return tk_down; }

void touch_clear_events(void)
{
    tk_press_evt = 0;
    tk_release_evt = 0;
}

uint8_t touch_take_press(void)
{
    uint8_t e = tk_press_evt;
    tk_press_evt = 0;
    return e;
}

uint8_t touch_take_release(void)
{
    uint8_t e = tk_release_evt;
    tk_release_evt = 0;
    return e;
}

/*
 * 保持時間は 16bit なので 65.5 秒で一周する。一周させないのは
 * 押しっぱなし救済 (TOUCH_STUCK_MS = 30 秒) の役目で、そちらが先に
 * 走って離しを立てるため、ここが 65535 に届くことはない。
 * TOUCH_STUCK_MS を 65 秒より先に延ばすなら、この型も一緒に広げること。
 * 一周すると保持時間が 0 に戻るので、長押しの途中で DFU の判定が
 * 外れ、押し続けているのに短押しとして扱われる。
 */
uint16_t touch_hold_ms(void)
{
    if (!tk_down) {
        return 0;
    }
    return (uint16_t)(tick_now() - tk_down_since);
}

uint16_t touch_raw(void) { return tk_filtered; }
uint16_t touch_baseline(void) { return tk_baseline; }
uint16_t touch_noise(void) { return tk_noise; }
uint16_t touch_peak_delta(void) { return tk_peak; }

uint16_t touch_delta(void)
{
    return (tk_baseline > tk_filtered) ? (uint16_t)(tk_baseline - tk_filtered)
                                       : 0;
}

uint16_t touch_threshold(void) { return tk_threshold; }
touch_thr_src_t touch_threshold_source(void) { return tk_src; }

void touch_set_threshold(uint16_t thr)
{
    /*
     * 下限は自動決定や学習と揃えること。ここだけ下限が無いと、
     * コンソールの - を押しすぎて閾値 1 まで落とせてしまう。
     * そうなるとノイズで勝手に押下判定が続き、モードが目まぐるしく
     * 変わり、保持時間が 3.6 秒を超えてブートローダに落ちる。
     * さらに s で焼いてしまうと、起動するたび同じ状態になる。
     *
     * 範囲外は**丸める**こと。拒否にすると境界に着地できない。
     * コンソールの +/- は 10 刻みなので、55 から - を押すと 45 に
     * なり、次は 35 が拒否されて 45 で止まる。下限が 40 なのに
     * 45 より下げられない。上も同じで、3995 から + を押すと 4005 が
     * 拒否されて 3995 に張り付く。飽和させれば境界ちょうどに乗る。
     */
    if (thr < TOUCH_THRESHOLD_MIN) {
        thr = TOUCH_THRESHOLD_MIN;
    } else if (thr > TOUCH_THRESHOLD_MAX) {
        thr = TOUCH_THRESHOLD_MAX;
    }
    tk_threshold = thr;

    /*
     * 保存はしないので、Data-Flash の記録とは食い違う。
     * ここを更新しないと、`+` を 1 回押しただけで `i` が
     * 「thr=310 src=flash」と表示し、実際の記録は 300 のまま、になる。
     */
    if (tk_src == TOUCH_THR_FLASH) {
        tk_src = TOUCH_THR_UNSAVED;
    }
}

uint16_t touch_peak_threshold(uint8_t divisor)
{
    uint16_t thr;

    if (divisor == 0) {
        return 0;
    }
    thr = (uint16_t)(tk_peak / divisor);

    /*
     * ピークが下限にも届かないような弱い当たり方だった場合は、
     * それを閾値にすると常時反応する石になってしまうので採用しない。
     */
    if (thr < TOUCH_THRESHOLD_MIN || thr > TOUCH_THRESHOLD_MAX) {
        return 0;
    }
    return thr;
}
