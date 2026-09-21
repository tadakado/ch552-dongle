/* SPDX-License-Identifier: MIT */
/*
 * neo.c - WS2812C-2020 ビットバンドライバ (P1.1 / 24MHz 前提)
 *
 * WS2812C-2020 のタイミング仕様 (Worldsemi のデータシート。上下限が
 * 直接与えられていて、WS2812B の「中心値 +-150ns」とは別物):
 *   T0H 220-380ns    T0L 580-1000ns
 *   T1H 580-1000ns   T1L 580-1000ns
 *   800kbps (公称周期 1250ns)   リセット Trst >= 280us
 * (Trst は WS2812B の 50us より長い。ここは C-2020 固有なので注意)
 *
 * 24MHz では 1 クロック = 41.7ns。CH55x の命令周期には明快な規則があり、
 * WCH の「CH55X 汇编指令说明」に次のように書かれている。
 *
 *   - 非跳転命令の周期数 = 命令バイト数 (CH551/2/3/4 の DIV を除く)
 *   - DJNZ/JB/JNB/JBC/CJNE A,dir は「バイト数 + 2」から。本命令の
 *     アドレスが奇数なら +1、分岐先が奇数ならさらに +1
 *   - 条件分岐が成立しなければバイト数と同じ
 *
 * つまり NOP 数は当てずっぽうではなく計算できる。使う命令は
 *   NOP 1B/1cyc, SETB bit 2B/2cyc, CLR bit 2B/2cyc,
 *   MOV bit,C 2B/2cyc, RLC A 1B/1cyc, DJNZ Rn,rel 2B/4-6cyc
 * なので、
 *   T0H    = A + 2
 *   T1H    = A + B + 4
 *   周期    = A + B + C + 11 (+1 DJNZ のアドレスが奇数のとき)
 * となる。tools/neo_timing.py が生成物のリストファイルから同じ計算を
 * するので、NOP 数を変えたら make timing で確かめられる。
 *
 * ただしピンが変化するのが命令の最終サイクルという前提を置いているので、
 * 実測とは 1 サイクル (約 42ns) ずれる余地がある。最終的にはロジアナで
 * T0H / T1H を当たること。
 *
 * ビット生成の考え方:
 *   setb PIN        立ち上がり
 *   ... A ...       T0H ぶん待つ
 *   mov  PIN, C     ビットが 0 ならここで立ち下がる
 *   ... B ...       T1H - T0H ぶん待つ
 *   clr  PIN        ビットが 1 ならここで立ち下がる
 *   ... C ...       周期の残り
 * 分岐を使わないので 0 と 1 で経路長が変わらないのが利点。
 */
#include "ch552.h"
#include "config.h"
#include "neo.h"
#include "sys.h"

/* アセンブラから触るビット記号。config.h の NEO_BIT に合わせる */
#if NEO_BIT == 0
#define NEO_PIN_ASM _P1_0
#elif NEO_BIT == 1
#define NEO_PIN_ASM _P1_1
#elif NEO_BIT == 4
#define NEO_PIN_ASM _P1_4
#elif NEO_BIT == 5
#define NEO_PIN_ASM _P1_5
#elif NEO_BIT == 6
#define NEO_PIN_ASM _P1_6
#elif NEO_BIT == 7
#define NEO_PIN_ASM _P1_7
#else
#error "NEO_BIT: サポート外のビット番号"
#endif

/*
 * NOP 数。規格の 4 項目すべてについて、上下限からの余裕が最大になる
 * 組を総当たりで選んである。
 *
 *   24MHz (6/5/7): T0H 333 (+47) / T1H 625 (+45) / T0L 917 (+83) /
 *                  T1L 625 (+45) / 周期 1250
 *   16MHz (3/3/2): T0H 312 (+68) / T1H 625 (+45) / T0L 938 (+62) /
 *                  T1L 625 (+45) / 周期 1250
 *
 * T1H が下限 580ns より十分上にあるのは、WS2812 系の実チップが
 * 立ち上がりから一定時間後の 1 点でサンプルしていて、その閾値が
 * 500ns 付近にあるという報告があるため (cpldcpu / josh.com)。
 * 625ns なら規格にも実装にも余裕がある。
 */
#if FREQ_SYS == 24000000UL
/*
 * WS2812C-2020 の実データシートは
 *   T0H 220-380ns / T1H 580-1000ns / T0L 580-1000ns / T1L 580-1000ns
 *   RES > 280us、800kbps (公称周期 1250ns)
 * で、WS2812B の「中心値 +-150ns」とは別物。
 *
 * 以前は B の表記 (T0H 0.30+-0.15 / T1H 0.60+-0.15 / T1L 0.30+-0.15) を
 * C-2020 の値だと思って詰めていて、A=5 B=7 C=6 だと T1L が 583ns、
 * 下限 580ns に対して **3ns** しか余裕が無かった。本ツールが自ら
 * 「実測とは 1 サイクル (42ns) ずれる余地がある」と断っている以上、
 * 3ns は余裕とは呼べない。
 *
 * A=6 B=5 C=7 にすると
 *   T0H 333 / T1H 625 / T0L 917 / T1L 625 / 周期 1250
 * で最悪マージンが 45ns。1 サイクルぶんを超えるので、机上の
 * 不確かさを吸収できる。全組み合わせを総当たりした最良値。
 */
#define NEO_A_NOPS 6
#define NEO_B_NOPS 5
#define NEO_C_NOPS 7
#elif FREQ_SYS == 16000000UL
#define NEO_A_NOPS 3
#define NEO_B_NOPS 3
#define NEO_C_NOPS 2
#else
#error "WS2812 のタイミングは 16MHz / 24MHz のみ用意してある"
#endif

/*
 * neo_show() は 24bit ぶん (LED 1 個) しか送らない。config.h の
 * NEO_COUNT を増やしても連結先には届かないので、機械に止めさせる。
 * 「2 にしたのに 2 個目が光らない」で悩まないように。
 */
#if NEO_COUNT != 1
#error "neo_show() は LED 1 個ぶんしか送らない。増やすなら neo.c も直すこと"
#endif

/* 送出対象。asm から名前で参照するのでグローバルに置く */
static __data uint8_t neo_byte;

/*
 * __naked かつ本体が __asm だけの関数は、SDCC から見て「レジスタを
 * 一つも使わない関数」に見える。呼び出し側は退避コードを出さないので、
 * ここで壊したレジスタは呼び出し側の生きた値を巻き込む。
 *
 * 実際に踏んだ: neo_show() が赤成分を R7 に置いたまま緑を送りに来ていて、
 * ここの djnz r7 が R7 をちょうど 0 にして返すため、赤が常に 0 になっていた。
 * モード B (FF,00,00) が完全に消灯し、白はシアン、マゼンタは青になる。
 * 緑と青だけが正しく出るので気づきにくい。
 *
 * 使うものは自分で退避する。A / PSW(C) / R7 の 3 つ。
 * push/pop はビットループの外なので、ビットタイミングには影響しない。
 * バイト間の低区間が 12 サイクル (0.5us) 伸びるだけで、
 * ラッチ判定の 280us には遠く及ばない。
 */
static void neo_send_byte(void) __naked
{
    /*
     * マクロは cpp が展開してからアセンブラに渡る。
     *
     * CH55x の djnz は本命令と分岐先の両方の奇偶が効く (どちらかが
     * 奇数番地なら +1)。つまり周期はリンク配置で動きうるのだが、
     * **分岐距離が奇数なら、どこに置かれても必ず片方だけが奇数になり、
     * 常に +1 ちょうど**になる。ここは
     *   rlc(1) + setb(2) + A + mov(2) + B + clr(2) + C = 7 + A+B+C
     * で、A+B+C = 18 なので 25 バイト = 奇数。配置不変。
     * NOP の個数を変えるときは合計が偶数になるようにすること
     * (make timing が距離の奇偶も見ている)。
     *
     * 以前ここに .even を置いて「偶数番地に固定した」つもりでいたが、
     * .even はモジュール内オフセットの整列でしかなく、CSEG の基底が
     * 奇数 (実測 0x00B1) なら結果も奇数になる。何も固定できていなかった。
     *
     * 注意: __asm ブロックの中に日本語コメントを書くと SDCC が
     * バイトを落としてアセンブルエラーになる。説明はここに書くこと。
     */
    __asm
        push acc
        push psw
        push ar7
        mov  a, _neo_byte
        mov  r7, #8
    00001$:
        rlc  a                    ; MSB を C へ。WS2812 は MSB first
        setb NEO_PIN_ASM          ; 立ち上がり
        .rept NEO_A_NOPS
        nop
        .endm
        mov  NEO_PIN_ASM, c       ; 0 ならここで落ちる (T0H)
        .rept NEO_B_NOPS
        nop
        .endm
        clr  NEO_PIN_ASM          ; 1 ならここで落ちる (T1H)
        .rept NEO_C_NOPS
        nop
        .endm
        djnz r7, 00001$
        pop  ar7
        pop  psw
        pop  acc
        ret
    __endasm;
}

void neo_init(void)
{
    /* プッシュプル出力。WS2812 のデータ線はオープンドレインでは駆動できない */
    /*
     * 順序が大事。P1 のリセット値は 0xFF なので、ラッチを落とす前に
     * P1_DIR_PU でプッシュプルにすると、数クロックのあいだ H を
     * 能動的に駆動してしまう。24MHz で約 125ns。C-2020 の T0H は
     * 220〜380ns なのでその下ではあるが、下限を割ったパルスを
     * どう扱うかは規定されていない。拾われると 1 ビットずれた状態で
     * 最初のフレームを受け取り、消灯させるつもりが変な色で光る。
     * 先に落とす。
     */
    NEO_PORT &= ~(1 << NEO_BIT);
    P1_MOD_OC &= ~(1 << NEO_BIT);
    P1_DIR_PU |= (1 << NEO_BIT);

    /*
     * 送る前に 280us 以上の Low を作る。**電源投入だけを考えると要らないが、
     * リセットでは要る**。P1 のリセット値は 0xFF なので、ウォッチドッグや
     * ソフトリセットで戻ってきたとき、電源が切れていない WS2812 は
     * 「1ms ほど H に張り付いた線」を見せられた直後にこのフレームを
     * 受け取ることになる。リセット区間を挟まないと、24bit はビットが
     * ずれた状態で解釈されるか 2 個目の LED ぶんとして DOUT に流れ、
     * 消灯させるつもりが変な色のまま touch_init() の 72〜720ms を
     * 光り続ける。ちょうどこの下のコメントが防ぎたいと言っている状態。
     */
    neo_latch();

    /*
     * 黒を 1 フレーム送って消灯を確定させる。
     * ピンの向きを決めただけでは WS2812 は電源投入時の状態のままで、
     * 最初のフレームを送るまで何色で光るか保証がない。ここまでに
     * タッチの較正で 72ms ほど待つので、その間ずっと不定色で
     * 光っていることになってしまう。
     */
    neo_show(0, 0, 0, 0);
}

void neo_show(uint8_t r, uint8_t g, uint8_t b, uint8_t level)
{
    uint8_t saved_ea = EA;

    /*
     * 色 x 明るさ。0xFF x 0x40 >> 8 = 0x3F なので上限は NEO_MAX 近傍。
     * この書き方 (両辺を uint8_t にキャストしてから uint16_t で受ける) だと
     * SDCC が 8x8 の MUL AB 1 命令に落としてくれる。素直に
     * (uint16_t)g * level と書くと __mulint 呼び出しになって数十倍遅い。
     */
    uint8_t vg = (uint8_t)((uint16_t)((uint8_t)g * (uint8_t)level) >> 8);
    uint8_t vr = (uint8_t)((uint16_t)((uint8_t)r * (uint8_t)level) >> 8);
    uint8_t vb = (uint8_t)((uint16_t)((uint8_t)b * (uint8_t)level) >> 8);

    EA = 0;
    neo_byte = vg; /* WS2812 は GRB 順 */
    neo_send_byte();
    neo_byte = vr;
    neo_send_byte();
    neo_byte = vb;
    neo_send_byte();
    EA = saved_ea;

    /*
     * ラッチ待ちは呼び出し側のフレーム間隔 (NEO_FRAME_MS) が
     * 280us を大きく上回るので、ここでは待たない。
     * 続けて 2 回送る場合だけ neo_latch() を挟むこと。
     */
}

void neo_latch(void)
{
    /*
     * WS2812C-2020 のリセットは 280us 以上 (B の 50us とは違う)。
     * これを置かずに 2 回続けて送ると、2 回目の 24bit は
     * 「2 個目の LED のデータ」と見なされて DOUT に素通りし、
     * 1 個しか繋いでいない構成では何も起きない。
     */
    delay_us(NEO_LATCH_US);
}
