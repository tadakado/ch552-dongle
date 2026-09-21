/* SPDX-License-Identifier: MIT */
#include "ch552.h"
#include "config.h"
#include "sys.h"

void sys_init(void)
{
    uint8_t cfg;

    /*
     * CLOCK_CFG と GLOBAL_CFG はセーフモード中しか書けない。
     * 0x55, 0xAA を連続で書くと「約 13〜23 システムクロック」だけ
     * 解錠され、そのあいだは 1 つ以上のレジスタを書き換えてよい
     * (データシート SAFE_MOD の項)。命令数で決まる窓ではない。
     */
    /*
     * bROM_CLK_FAST も同じレジスタなので、1 回の書き込みでまとめて決める。
     * このビットの基準は **Fosc (発振器そのもの)** であって Fsys では
     * ない。データシートも WCH のヘッダも "0=normal(for Fosc>=16MHz),
     * 1=fast(for Fosc<16MHz)" と書いている。内蔵発振器を使う限り
     * Fosc は分周前の 24MHz 固定なので、Fsys を 16MHz に落としても
     * 0 のままが正しい。「Fsys で決める」と思い込むと、将来 12MHz を
     * 足したときにここを 1 にしてしまう。
     */
#if FREQ_SYS == 24000000UL
#define CLOCK_CFG_VAL SYS_CK_24M
#elif FREQ_SYS == 16000000UL
#define CLOCK_CFG_VAL SYS_CK_16M
#else
/*
 * 12MHz 以下は対応していない。WS2812 の NOP 数 (neo.c) も
 * delay_us の NOP 数もその周波数ぶんを用意していないし、
 * delay_us の外側ループが 16 クロックあるので 1us を切れない。
 * 増やすならこの 3 箇所を揃えて直すこと。
 */
#error "FREQ_SYS must be 16000000 or 24000000"
#endif

    /*
     * 書き込む値は解錠の前に作っておき、解錠後は 1 命令で書き終える。
     *
     * anl / orl の 2 命令に分けると、1 発目の anl で SYS_CK_SEL が 000
     * = 187.5kHz になり、2 発目の orl はその遅いクロックで実行される。
     * セーフモードの有効期間は「約 13〜23 システムクロック」とされるが、
     * それが分周後のクロックで数えられている保証はない。発振器クロック
     * 基準だとしたら 2 発目は窓の外に落ち、**187.5kHz のまま固まる**。
     * そうなると呼吸は 128 倍遅く、WS2812 は化け、tick も USB も総崩れで、
     * しかも原因が「クロック設定の 2 命令目」だとは気づきにくい。
     *
     * 値を先に作れば中間状態も窓の問題も消える。WCH の例も ch55xduino も
     * 1 回書きなので、そちらに揃えておく。
     */
    cfg = (uint8_t)((CLOCK_CFG & ~(MASK_SYS_CK_SEL | bROM_CLK_FAST)) |
                    CLOCK_CFG_VAL);

    SAFE_MOD = 0x55;
    SAFE_MOD = 0xAA;
    CLOCK_CFG = cfg;
    SAFE_MOD = 0x00;
}

/* ------------------------------------------------------------------ */
/* 空ループの待ち                                                      */
/* ------------------------------------------------------------------ */
/*
 * C で書くと SDCC が出すループの形が版や最適化で変わり、実際に何
 * クロックかかるか読めない。以前は「内側ループ 1 周 = 4 クロック」
 * という前提で係数を決めていたが、生成コードは 11 クロックあり、
 * 24MHz で 3.83 倍も長かった。ここではループの形をアセンブラで
 * 固定して、クロックを数えて合わせる。
 *
 * ただしそれだけでは足りない。CH55x の命令周期には奇偶の項がある。
 *
 *   JB/JNB/JBC/CJNE A,dir/DJNZ/JMP@A  本命令が奇数番地で +1、
 *                                     分岐先が奇数番地でさらに +1
 *   その他の跳転命令                  分岐先が奇数番地なら +1
 *
 * つまりループの周期がリンク配置で変わる。以前ここに `.even` を
 * 置いて「偶数番地に固定した」つもりでいたが、**`.even` はモジュール内
 * オフセットの整列でしかなく、CSEG の基底が奇数なら結果も奇数になる**。
 * 実際 s_CSEG = 0x00B1 (奇数) で、何も固定できていなかった。
 *
 * そこで番地を固定するのではなく、**配置が変わっても周期が変わらない
 * 形**にする。DJNZ は本命令と分岐先の両方の奇偶が効くので、
 * **分岐距離を奇数にしておけば、どちらに置かれても必ず片方だけが
 * 奇数になり、常に +1 ちょうど**になる。
 *
 *   1 周 = DELAY_US_NOPS + djnz(2 + 2 + 1) = DELAY_US_NOPS + 5
 *
 * 分岐距離は NOP の個数そのものなので、DELAY_US_NOPS を奇数にすれば
 * 条件を満たす。24MHz なら 24 - 5 = 19、16MHz なら 16 - 5 = 11。
 * どちらも奇数で、ちょうど 1us になる。
 *
 * 16bit の反復には djnz の 2 段重ねを使う。r6 が下位、r7 が上位 + 1。
 * 下位が 0 のとき (256 の倍数) だけ +1 しないよう補正する。
 * 上位側の djnz は 256 回に 1 回しか回らないので、その 5 サイクルは
 * 1 反復あたり 0.02 サイクル、delay_ms(100) で +0.08ms。
 *
 * tools/delay_check.py が生成コードからサイクル数を数え直し、
 * 分岐距離が奇数であること (= 配置不変であること) も確かめる。
 */
#if FREQ_SYS == 24000000UL
#define DELAY_US_NOPS 19
#elif FREQ_SYS == 16000000UL
#define DELAY_US_NOPS 11
#else
#error "delay_us は 16MHz / 24MHz のみ用意してある"
#endif

/*
 * __naked かつ本体が __asm だけなので、SDCC はこの関数が
 * 「レジスタを何も使わない」と思い込み、呼び出し側は退避を出さない。
 * (neo_send_byte で実際に赤成分を壊した。) 使うものは自分で退避する。
 *
 * 注意: __asm ブロックの中に日本語コメントを書くと SDCC がバイトを
 * 落としてアセンブルエラーになる。説明は C のコメント側に書くこと。
 */
void delay_us(uint16_t us) __naked
{
    (void)us;
    __asm
        push acc
        push psw
        push ar6
        push ar7
        mov  a, dpl
        orl  a, dph
        jz   00003$
        mov  r6, dpl
        mov  r7, dph
        mov  a, dpl
        jz   00004$
        inc  r7
    00004$:
    00001$:
        .rept DELAY_US_NOPS
        nop
        .endm
        djnz r6, 00001$
        djnz r7, 00001$
    00003$:
        pop  ar7
        pop  ar6
        pop  psw
        pop  acc
        ret
    __endasm;
}

void delay_ms(uint16_t ms)
{
    while (ms--) {
        delay_us(1000);
    }
}

void sys_watchdog_init(void)
{
#if WATCHDOG_ENABLE
    uint8_t saved_ea = EA;

    /* セーフモードの解錠は連続した 2 回の書き込みが条件 (dflash.c 参照) */
    EA = 0;
    WDOG_COUNT = 0x00;
    SAFE_MOD = 0x55;
    SAFE_MOD = 0xAA;
    GLOBAL_CFG |= bWDOG_EN;
    SAFE_MOD = 0x00;
    EA = saved_ea;
#endif
}

void sys_watchdog_feed(void)
{
#if WATCHDOG_ENABLE
    WDOG_COUNT = 0x00;
#endif
}

#if WATCHDOG_ENABLE
static void watchdog_disable(void)
{
    SAFE_MOD = 0x55;
    SAFE_MOD = 0xAA;
    GLOBAL_CFG &= ~bWDOG_EN;
    SAFE_MOD = 0x00;
}
#endif

/*
 * ホストのサスペンド中に眠る。
 *
 * **EA = 0 の状態で呼ぶこと。戻るときも EA = 0 のまま。**
 *
 * 「サスペンド中か」の判定とここに来るまでの間にレジュームが来ると、
 * ホストが起きているのにこちらだけ眠ることになる。以前はこの関数の
 * 中で EA を落としていたので、呼び出し側の判定は割り込み有効のまま
 * 行われていて、その隙間が塞がっていなかった。判定ごと呼び出し側の
 * クリティカルセクションに入れてもらう形にしてある (main.c 参照)。
 *
 * EA=0 でもハードウェアの起床は効く (PD は起床時に自動でクリアされる)。
 * 溜まっていた USB 割り込みは、呼び出し側が EA を戻した時点で走る。
 * 仮に判定をすり抜けて眠ったとしても、ホストがレジュームすれば
 * 1ms ごとに SOF が流れるので bWAK_BY_USB で起きる。
 */
void sys_sleep(void)
{
#if WATCHDOG_ENABLE
    /*
     * 眠っている間はクロックが止まるのでウォッチドッグも止まるはずだが、
     * そこに賭ける理由が無いので明示的に止めておく。
     * 起床後に必ず入れ直す。
     */
    WDOG_COUNT = 0x00;
    watchdog_disable();
#endif

    SAFE_MOD = 0x55;
    SAFE_MOD = 0xAA;
    WAKE_CTRL = bWAK_BY_USB;
    SAFE_MOD = 0x00;

    PCON |= PD;   /* ここで眠る。USB のバス活動で起きる */

    /*
     * 起床直後のダミー。データシートにあるのは「起床所要時間
     * Twak = 1 / 2 / 10us」というハードウェア側の値だけで、
     * ソフトで待てという記述は無い (待つ必要があるなら 24MHz の
     * NOP 4 個 = 167ns では桁が足りない)。害が無いので置いてあるが、
     * これで何かを保証しているつもりは無い。
     */
    __asm__("nop");
    __asm__("nop");
    __asm__("nop");
    __asm__("nop");

    SAFE_MOD = 0x55;
    SAFE_MOD = 0xAA;
    WAKE_CTRL = 0x00;
    SAFE_MOD = 0x00;

#if WATCHDOG_ENABLE
    WDOG_COUNT = 0x00;
    SAFE_MOD = 0x55;
    SAFE_MOD = 0xAA;
    GLOBAL_CFG |= bWDOG_EN;
    SAFE_MOD = 0x00;
#endif
}

void sys_jump_bootloader(void)
{
    /*
     * ch55xduino が 1200baud トリガで行っているのと同じ手順。
     *   1. USB を完全に止める (D+ プルアップも切れる)
     *   2. 割り込みとタイマを止める
     *   3. 100ms 待ってホストに切断を認識させる
     *   4. ブートローダ先頭 0x3800 へ lcall
     * この待ち時間を省くと「飛んだのに ISP ツールから見えない」に
     * なりやすい。順序そのものが肝。
     */
    USB_CTRL = 0x00;
    UDEV_CTRL = 0x00;
    EA = 0;

    /*
     * 割り込みの許可とフラグも落とす。EA = 0 のあいだは無害だが、
     * 0x0000 のベクタ表はこのファームのままなので、ブートローダが
     * どこかで EA を立てると溜まっていたフラグでこちらの usb_isr に
     * 飛び込む。ISP の最中に自分のハンドラが動くのは避けたい。
     * 個別のビットを消していくと消し忘れるので、IE / IE_EX ごと落とす。
     */
    USB_INT_EN = 0x00;
    USB_INT_FG = 0xFF;   /* 書き込みでクリアされるビットを掃除する */
    IE = 0x00;           /* EA も含めて全部。EA = 0 の代わりでもある */
    IE_EX = 0x00;

    /*
     * 割り込みを止めるだけでなく、周辺そのものも止める。タッチキーは
     * 1ms ごとに変換を回して bTKC_IF を立て続けるので、放っておくと
     * ISP の最中も P1.7 で容量測定が走ったままになる。
     */
    TKEY_CTRL = 0x00;
    ADC_CTRL = 0x00;

    /*
     * Timer0/1 の走行ビットは TCON の TR0 / TR1。TMOD はモードを
     * 選ぶだけなので、TMOD = 0 では止まらない。いまは Timer0/1 を
     * 使っていないので実害は無いが、「止めたつもり」を残さない。
     */
    TCON = 0x00;
    TMOD = 0x00;
    T2CON = 0x00;  /* Timer2 も止める */

#if WATCHDOG_ENABLE
    /*
     * ウォッチドッグは必ず止めてから飛ぶこと。bWDOG_EN はジャンプでは
     * クリアされないので、有効なまま渡すとブートローダが餌をやらずに
     * 約 700ms でリセットがかかる。書き込みの最中にリセットされたら
     * 目も当てられない。
     */
    watchdog_disable();
#endif

    delay_ms(100);

    __asm__("lcall #0x3800");

    /* 保険。ここには来ない */
    while (1) {
        ;
    }
}
