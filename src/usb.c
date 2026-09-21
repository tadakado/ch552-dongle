/* SPDX-License-Identifier: MIT */
/*
 * usb.c - CH552 USB デバイスコア (CDC-ACM + HID 複合)
 *
 * CH55x の SIE は抽象化されていないぶん、押さえる点は少ない。
 *
 *  - 端点バッファは xRAM 上の任意の偶数番地に置き、UEPn_DMA で指す。
 *  - 単一バッファモードでは、**有効にした方向のぶんだけ**が UEPn_DMA から
 *    低位→高位に 64 バイト単位で並ぶ (受信が先。表 16.3.3)。
 *    送信のみの端点は +0、受信も有効な端点は +64 が送信。
 *    一律 +64 ではない。配置は MOD レジスタの値から導いている。
 *  - UEPn_CTRL の応答種別は「次にホストへ返す handshake」を意味する。
 *    送るものが無い間は NAK にしておき、用意できたら ACK に変える。
 *  - EP0 には自動トグルが無いので、データステージの継続では
 *    bUEP_T_TOG を自分で反転させる。EP1-3 は bUEP_AUTO_TOG に任せる。
 *  - bUC_INT_BUSY を立てておくと、転送完了割込を処理している間は
 *    SIE が自動で NAK を返してくれる。取りこぼしがぐっと減る。
 */
#include "ch552.h"
#include "config.h"
#include "usb.h"
#include "usb_desc.h"
#include "cdc.h"
#include "sys.h"

/* ------------------------------------------------------------------ */
/* 端点バッファ                                                        */
/* ------------------------------------------------------------------ */
/*
 * xRAM の前半に固定で並べる。番地は偶数に揃えてある。
 * (データシートに UEPn_DMA を偶数に限る記述は見つかっていない。
 *  WCH の例が例外なく偶数なので合わせているだけで、静的表明は
 *  「意図せず奇数にならない」ための歯止め。根拠は弱い。)
 *
 * 単一バッファモード (bUEPn_BUF_MOD = 0) では、UEPn_DMA から
 * **有効にした方向のぶんだけ**が低位→高位に並ぶ (表 16.3.3)。
 * 受信 (OUT) が先、送信 (IN) が後。したがって
 *
 *   受信のみ  : +0 受信                   64 バイト
 *   送信のみ  : +0 **送信**               64 バイト
 *   受信+送信 : +0 受信 / +64 送信       128 バイト
 *
 * 送信位置を一律 +64 だと思い込んでいた時期があり、IN のみの EP1 が
 * 誰も書いていない領域を送っていた。列挙もモード表示も正常なのに
 * マウスだけ動かない、という切り分けの難しい壊れ方をする。
 * 二度と間違えないよう、offset も必要な大きさも MOD レジスタに書く
 * 値から導き、静的検査で突き合わせる。
 *
 *   0x0000 EP0  64B  制御 (OUT と IN で同じ 64 バイトを共用)
 *   0x0040 EP1  64B  HID レポート。IN のみ -> 送信は +0
 *   0x0080 EP2 128B  CDC バルク。IN と OUT -> 受信 +0 / 送信 +64
 *   0x0100 EP3  64B  CDC 通知。IN のみ -> 送信は +0 (実際には送らない)
 *   0x0140 ここから先は SDCC の xdata
 */
#define EP0_BUF_ADDR 0x0000
#define EP1_BUF_ADDR 0x0040
#define EP2_BUF_ADDR 0x0080
#define EP3_BUF_ADDR 0x0100

/*
 * 端点の方向設定。レジスタに書く値をここで決めて、
 * バッファの配置はすべてこの値から導く。
 */
#define UEP4_1_MOD_VAL (bUEP1_TX_EN)
#define UEP2_3_MOD_VAL (bUEP2_RX_EN | bUEP2_TX_EN | bUEP3_TX_EN)

#define EP1_RX_ON ((UEP4_1_MOD_VAL & bUEP1_RX_EN) ? 1 : 0)
#define EP1_TX_ON ((UEP4_1_MOD_VAL & bUEP1_TX_EN) ? 1 : 0)
#define EP2_RX_ON ((UEP2_3_MOD_VAL & bUEP2_RX_EN) ? 1 : 0)
#define EP2_TX_ON ((UEP2_3_MOD_VAL & bUEP2_TX_EN) ? 1 : 0)
#define EP3_RX_ON ((UEP2_3_MOD_VAL & bUEP3_RX_EN) ? 1 : 0)
#define EP3_TX_ON ((UEP2_3_MOD_VAL & bUEP3_TX_EN) ? 1 : 0)

/* 送信位置 = 受信を有効にしているなら +64、していないなら +0 */
#define EP1_TX_OFF (EP1_RX_ON * UEP_DIR_SIZE)
#define EP2_TX_OFF (EP2_RX_ON * UEP_DIR_SIZE)
#define EP3_TX_OFF (EP3_RX_ON * UEP_DIR_SIZE)

/* 必要な大きさ = 有効な方向の数 x 64 */
#define EP1_BUF_LEN ((EP1_RX_ON + EP1_TX_ON) * UEP_DIR_SIZE)
#define EP2_BUF_LEN ((EP2_RX_ON + EP2_TX_ON) * UEP_DIR_SIZE)
#define EP3_BUF_LEN ((EP3_RX_ON + EP3_TX_ON) * UEP_DIR_SIZE)

__xdata __at(EP0_BUF_ADDR) uint8_t ep0_buf[EP0_SIZE];
__xdata __at(EP1_BUF_ADDR) uint8_t ep1_buf[EP1_BUF_LEN];
__xdata __at(EP2_BUF_ADDR) uint8_t ep2_buf[EP2_BUF_LEN];
__xdata __at(EP3_BUF_ADDR) uint8_t ep3_buf[EP3_BUF_LEN];

/*
 * __at で置いた変数は SDCC の xdata 割り当てに含まれないので、
 * 何も言わないでいると普通の xdata 変数がここに重なる。
 * Makefile の --xram-loc で SDCC の領域を USB_RAM_END 以降に追い出し、
 * その約束が守られていることをここで静的に検査する。
 *
 * 併せて、端点どうしが重ならないことと番地が偶数であることも見る。
 * 手で並べた住所なので、サイズを変えたときにここが効く。
 */
typedef char usb_ram_even[
    (((EP0_BUF_ADDR | EP1_BUF_ADDR | EP2_BUF_ADDR | EP3_BUF_ADDR) & 1) == 0)
    ? 1 : -1];
typedef char usb_ram_ep1[(EP0_BUF_ADDR + EP0_SIZE <= EP1_BUF_ADDR) ? 1 : -1];
typedef char usb_ram_ep2[(EP1_BUF_ADDR + EP1_BUF_LEN <= EP2_BUF_ADDR) ? 1 : -1];
typedef char usb_ram_ep3[(EP2_BUF_ADDR + EP2_BUF_LEN <= EP3_BUF_ADDR) ? 1 : -1];
typedef char usb_ram_fits[
    (EP3_BUF_ADDR + EP3_BUF_LEN <= USB_RAM_END) ? 1 : -1];

/* 送るパケットが、その端点の送信領域に収まること */
typedef char usb_ep1_tx_fits[
    (EP1_TX_OFF + EP1_SIZE <= EP1_BUF_LEN) ? 1 : -1];
typedef char usb_ep2_tx_fits[
    (EP2_TX_OFF + EP2_SIZE <= EP2_BUF_LEN) ? 1 : -1];
typedef char usb_ep3_tx_fits[
    (EP3_TX_OFF + EP3_SIZE <= EP3_BUF_LEN) ? 1 : -1];

/* ------------------------------------------------------------------ */
/* 内部状態                                                            */
/* ------------------------------------------------------------------ */
/*
 * 割り込み (usb_isr) とメインループの両方が触るものは volatile。
 * 今はアクセサが別の翻訳単位にあるので SDCC が毎回読み直しており、
 * 付けなくても動いてしまう。だがアクセサをヘッダに移して
 * インライン化した瞬間に、while (!usb_ep1_ready()) が無限ループに
 * なるたぐいの壊れ方をする。tick.c と cdc.c は最初から付けてある。
 */
static __code const uint8_t *desc_ptr;
static uint16_t setup_len;
static uint8_t  setup_req;
static uint8_t  setup_type;
static uint8_t  setup_iface;
static uint8_t  addr_pending;
static volatile uint8_t cfg_value;
static volatile uint8_t ep1_busy;
static volatile uint8_t ep2_busy;
static volatile uint8_t suspended;
static volatile uint8_t led_report;
static uint8_t  hid_idle_rate;
static uint8_t  hid_protocol = 1; /* 1 = Report プロトコル */

/*
 * CDC のラインコーディング。先頭 4 バイトがボーレート
 * (リトルエンディアン)、続いて stop bits / parity / data bits。
 * 既定は 115200-8N1 にしておく。
 */
static __xdata uint8_t line_coding[7] = {
    0x00, 0xC2, 0x01, 0x00, 0x00, 0x00, 0x08
};
static volatile uint8_t control_line_state;
static volatile uint8_t dfu_pending;
static volatile uint8_t remote_wakeup;   /* ホストが許可したか */

/* 標準リクエスト */
#define REQ_GET_STATUS        0x00
#define REQ_CLEAR_FEATURE     0x01
#define REQ_SET_FEATURE       0x03
#define REQ_SET_ADDRESS       0x05
#define REQ_GET_DESCRIPTOR    0x06
#define REQ_GET_CONFIGURATION 0x08
#define REQ_SET_CONFIGURATION 0x09
#define REQ_GET_INTERFACE     0x0A
#define REQ_SET_INTERFACE     0x0B

/* HID クラスリクエスト */
#define HID_GET_REPORT        0x01
#define HID_GET_IDLE          0x02
#define HID_GET_PROTOCOL      0x03
#define HID_SET_REPORT        0x09
#define HID_SET_IDLE          0x0A
#define HID_SET_PROTOCOL      0x0B

/* CDC クラスリクエスト */
#define CDC_SET_LINE_CODING   0x20
#define CDC_GET_LINE_CODING   0x21
#define CDC_SET_CTRL_LINE     0x22
#define CDC_SEND_BREAK        0x23

#define CDC_DTR 0x01
#define CDC_RTS 0x02

/* SET_FEATURE / CLEAR_FEATURE の wValue */
#define FEATURE_DEVICE_REMOTE_WAKEUP 0x01
#define FEATURE_ENDPOINT_HALT        0x00

#define STALL 0xFF

/*
 * 端点を既定状態に戻す。ハルト解除、応答 NAK/ACK、データトグルは
 * DATA0 (bUEP_*_TOG を落とす)。
 *
 * バスリセットのほか SET_CONFIGURATION / SET_INTERFACE でも必要。
 * USB 2.0 の 9.4.7 / 9.4.10 で、これらのあと端点は非ハルトかつ
 * DATA0 と決められている。列挙の途中はバスリセットが先行するので
 * 忘れていても動くが、libusb などがポートリセット無しに
 * SET_CONFIGURATION を投げると、ホストだけ DATA0 に戻って
 * こちらは前のトグルのまま、という食い違いが起きる。
 * 症状は「レポートが 1 個おきに落ちる」で、原因が見えにくい。
 */
static void ep1_reset(void)
{
    UEP1_CTRL = bUEP_AUTO_TOG | UEP_T_RES_NAK;
    UEP1_T_LEN = 0;
    ep1_busy = 0;
}

static void ep2_reset(void)
{
    UEP2_CTRL = bUEP_AUTO_TOG | UEP_T_RES_NAK | UEP_R_RES_ACK;
    UEP2_T_LEN = 0;
    ep2_busy = 0;
    cdc_reset();
    /* 空きを見て ACK/NAK を決め直す (捨てるのはメインなのでまだ満杯) */
    cdc_rx_rearm();
}

static void ep3_reset(void)
{
    UEP3_CTRL = bUEP_AUTO_TOG | UEP_T_RES_NAK;
    UEP3_T_LEN = 0;
}

/* 全端点。バスリセットと SET_CONFIGURATION 用 */
static void ep_reset(void)
{
    ep1_reset();
    ep2_reset();
    ep3_reset();
}

/*
 * SET_INTERFACE で初期化するのは、名指しされたインタフェースが
 * 持つ端点だけ。全部落とすと、HID 宛ての要求で CDC の送信待ちが
 * 消えたりトグルがずれたりする。
 */
static void ep_reset_iface(uint8_t iface)
{
    switch (iface) {
    case IF_CDC_CTRL: ep3_reset(); break;  /* 通知端点 */
    case IF_CDC_DATA: ep2_reset(); break;  /* バルク */
    case IF_HID:      ep1_reset(); break;  /* 割り込み IN */
    default: break;
    }
}

/* 端点アドレスからハルト状態を読む。GET_STATUS(endpoint) 用 */
static uint8_t ep_halted(uint8_t addr)
{
    switch (addr) {
    case 0x81: return (uint8_t)((UEP1_CTRL & MASK_UEP_T_RES) == UEP_T_RES_STALL);
    case 0x82: return (uint8_t)((UEP2_CTRL & MASK_UEP_T_RES) == UEP_T_RES_STALL);
    case 0x02: return (uint8_t)((UEP2_CTRL & MASK_UEP_R_RES) == UEP_R_RES_STALL);
    case 0x83: return (uint8_t)((UEP3_CTRL & MASK_UEP_T_RES) == UEP_T_RES_STALL);
    default:   return 0xFF; /* 未知の端点 */
    }
}

/*
 * __code から ep0_buf への転送。memcpy は汎用ポインタを要求するので使わない。
 *
 * **引数を取らない**のは意図的。SDCC は非再入関数の引数とローカルを
 * OSEG (オーバレイ領域) に置き、リンカは全モジュールの OSEG を同じ番地に
 * 重ねる。この関数は ISR から呼ばれるので、そこに引数を置くと、メイン側で
 * 走っているライブラリ関数の引数 (__divulong_PARM_2 など。同じ番地) を
 * 割り込みが踏み潰す。32bit 除算は除数を計算ループの中で何度も読み直すので、
 * 窓は呼び出しの一瞬ではなく 100us 規模になる。
 *
 * 実際に重なっていた組み合わせ:
 *   main.c proximity_level() / mode.c の 1000 除算  -> __divulong_PARM_2
 *   cdc.c  cdc_put_u16()                            -> __divuint / __moduint
 *   touch.c tk_peak / divisor                       -> __divuint_PARM_2
 * 症状は「LED が 1 フレームだけ変な明るさ」「コンソールの数字が化ける」
 * 「学習が理由も無く失敗する」で、どれも再現しないので追いにくい。
 *
 * dst を __xdata 修飾しているのも同じ理由。無修飾ポインタは汎用ポインタに
 * なり、書き込みが __gptrput の呼び出しになる。その引数も OSEG の同じ番地。
 *
 * ここを触るときは make oseg で確かめること。
 */
static __data uint8_t copy_len;

static void copy_desc_to_ep0(void)
{
    __xdata uint8_t *dst = ep0_buf;
    __code const uint8_t *src = desc_ptr;
    uint8_t n = copy_len;

    while (n--) {
        *dst++ = *src++;
    }
}

/* ------------------------------------------------------------------ */
/* 初期化                                                              */
/* ------------------------------------------------------------------ */
void usb_init(void)
{
    USB_CTRL = 0x00;

    /*
     * D+ プルアップを上げる前にシリアル記述子を作る。
     * 列挙が始まってから作ると GET_DESCRIPTOR に間に合わない。
     */
    usb_desc_init_serial();

    UEP0_DMA_L = (uint8_t)((uint16_t)ep0_buf & 0xFF);
    UEP0_DMA_H = (uint8_t)((uint16_t)ep0_buf >> 8);
    UEP1_DMA_L = (uint8_t)((uint16_t)ep1_buf & 0xFF);
    UEP1_DMA_H = (uint8_t)((uint16_t)ep1_buf >> 8);
    UEP2_DMA_L = (uint8_t)((uint16_t)ep2_buf & 0xFF);
    UEP2_DMA_H = (uint8_t)((uint16_t)ep2_buf >> 8);
    UEP3_DMA_L = (uint8_t)((uint16_t)ep3_buf & 0xFF);
    UEP3_DMA_H = (uint8_t)((uint16_t)ep3_buf >> 8);

    UEP4_1_MOD = UEP4_1_MOD_VAL; /* EP1 は IN のみ。EP4 は使わない */
    UEP2_3_MOD = UEP2_3_MOD_VAL;

    UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
    UEP1_CTRL = bUEP_AUTO_TOG | UEP_T_RES_NAK;
    UEP2_CTRL = bUEP_AUTO_TOG | UEP_T_RES_NAK | UEP_R_RES_ACK;
    UEP3_CTRL = bUEP_AUTO_TOG | UEP_T_RES_NAK;
    UEP1_T_LEN = 0;
    UEP2_T_LEN = 0;
    UEP3_T_LEN = 0;

    USB_DEV_AD = 0x00;
    cfg_value = 0;
    ep1_busy = 0;
    ep2_busy = 0;
    suspended = 0;
    led_report = 0;
    control_line_state = 0;
    dfu_pending = 0;
    remote_wakeup = 0;

    /* デバイスモード、内蔵プルアップ有効、割込処理中は自動 NAK */
    USB_CTRL = bUC_DEV_PU_EN | bUC_INT_BUSY | bUC_DMA_EN;
    UDEV_CTRL = bUD_PD_DIS | bUD_PORT_EN;

    USB_INT_FG = 0xFF;
    USB_INT_EN = bUIE_SUSPEND | bUIE_TRANSFER | bUIE_BUS_RST;
    IE_USB = 1;
}

uint8_t usb_is_configured(void) { return (uint8_t)(cfg_value != 0); }
uint8_t usb_is_suspended(void) { return suspended; }
uint8_t usb_dfu_requested(void) { return dfu_pending; }

/*
 * ホストが送ってきたキーボード LED (NumLock / CapsLock / ScrollLock) の
 * 状態。点灯させる LED は無いので使い道は無いが、コンソールの i で
 * 見えるようにしてある。立ち上げのとき「ホストがこちらをキーボードと
 * 認識して出力レポートを投げてきているか」を確かめる唯一の手がかり。
 */
uint8_t usb_last_led_report(void) { return led_report; }


uint8_t usb_remote_wakeup(void)
{
    if (!suspended || !remote_wakeup || cfg_value == 0) {
        return 0;
    }

    /*
     * ホストを起こすレジューム信号 = フルスピードのバス上に K 状態を作る。
     * bUD_LOW_SPEED は「1.5k のプルアップを D+ と D- のどちらに繋ぐか」
     * を選ぶビットなので (データシート bUC_DEV_PU_EN の項)、一時的に
     * 立てるとプルアップが D- に移り、ホストの 15k プルダウンとの
     * 分圧でフルスピードのバス上は K に見える。
     *
     * **未検証**。データシートにリモートウェイクアップの手順は載って
     * いない。ch55xduino はこのビットを速度選択にしか使っておらず、
     * SET_FEATURE(DEVICE_REMOTE_WAKEUP) の処理も空なので裏付けにならない。
     * さらにこれは「駆動した K」ではなく「プルアップで作った K」なので
     * 立ち上がりが RC で鈍る。ホストが拾うかどうかは実機で見ること。
     *
     * USB 規格ではバスがアイドルになってから 5ms 以上空けたうえで、
     * K を 1〜15ms 駆動することになっている。2ms は仕様内。
     */
    UDEV_CTRL |= bUD_LOW_SPEED;
    delay_ms(2);
    UDEV_CTRL &= ~bUD_LOW_SPEED;

    return 1;
}

uint8_t usb_cdc_dtr(void)
{
    return (uint8_t)((control_line_state & CDC_DTR) ? 1 : 0);
}


/* ------------------------------------------------------------------ */
/* 端点の送信                                                          */
/* ------------------------------------------------------------------ */
/*
 * 送れる状態か。ハルト中も見る。ここを見ないと、ホストが端点を
 * 止めているのに次のレポートで勝手に解除してしまう。
 * 端点を止めるのはホストの意思なので、こちらから戻してはいけない。
 */
uint8_t usb_ep1_ready(void)
{
    return (uint8_t)(cfg_value != 0 && !ep1_busy &&
                     (UEP1_CTRL & MASK_UEP_T_RES) != UEP_T_RES_STALL);
}
uint8_t *usb_ep1_tx_buf(void) { return &ep1_buf[EP1_TX_OFF]; }

/*
 * 端点の応答ビットを触るところは、メインから呼ぶぶんは必ず EA で囲う。
 * UEPn_CTRL の読み込みと書き戻しのあいだに割り込みが入ると、
 * その中で書いた値を古い値で上書きしてしまう。
 *
 * 実際にありうる筋道:
 *   - EP2: ISR の cdc_on_rx() が受信リング満杯で R 応答を NAK に落とす
 *          -> main が cdc_flush() から commit してきて ACK のまま書き戻す
 *          -> NAK が消え、次のパケットを受け取って捨てる。
 *          「貼り付けの先頭だけ届く」という、塞いだはずの症状が戻る。
 *   - EP1: ホストが SET_FEATURE(ENDPOINT_HALT) を投げた直後に main が
 *          commit すると、ISR が立てた STALL を握り潰す。
 *          usb.c の GET_STATUS のコメントが警戒している状態そのもの。
 */
void usb_ep1_commit(uint8_t len)
{
    uint8_t saved_ea = EA;

    EA = 0;
    /*
     * 割り込み禁止に入るまでのあいだにホストが端点を止めた場合は
     * そのままにする。このパケットは捨てることになるが、
     * ハルトを握り潰すよりよい。
     */
    if ((UEP1_CTRL & MASK_UEP_T_RES) != UEP_T_RES_STALL) {
        ep1_busy = 1;
        UEP1_T_LEN = len;
        UEP1_CTRL = (uint8_t)((UEP1_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_ACK);
    }
    EA = saved_ea;
}

uint8_t usb_ep2_ready(void)
{
    return (uint8_t)(cfg_value != 0 && !ep2_busy &&
                     (UEP2_CTRL & MASK_UEP_T_RES) != UEP_T_RES_STALL);
}
uint8_t *usb_ep2_tx_buf(void) { return &ep2_buf[EP2_TX_OFF]; }

/*
 * これだけは ISR (cdc_rx_rearm 経由) からもメインからも呼ばれる。
 * 中で saved_ea を戻すので、ISR から呼ぶと ISR の途中で EA = 1 に戻る。
 * それが安全なのは **IP / IP_EX をどこでも書いていない**からで、
 * 両方の割り込みが優先度 0 にいる限り、実行中フラグが多重割り込みを
 * 止める (EA ではなく)。Timer2 を高優先度に上げると、ここが多重割り込みの
 * 窓になる。上げるならこの関数を EA 復元無しに直すこと。
 */
void usb_ep2_rx_set(uint8_t accept)
{
    uint8_t saved_ea = EA;

    /*
     * EP2 OUT の応答を ACK / NAK で切り替える。受信リングに
     * 1 パケット (64 バイト) ぶんの空きが無いときは NAK にして、
     * ホストに保持させる。
     *
     * これを入れる前は常に ACK で、入り切らなかったぶんは黙って
     * 捨てていた。SIE は ACK を返しているのでホストは書き込みが
     * 成功したと思い、再送しない。端末に 1 行貼り付けると
     * 先頭の十数バイトだけが届いて残りが消える、という形で出る。
     *
     * 割り込みの中 (cdc_on_rx) とメインの両方から呼ぶので、
     * リード・モディファイ・ライトは EA で囲う。割り込みの中で
     * 呼んだ場合は EA が 1 のまま読めるが、落として戻すだけなので
     * どちらの文脈でも正しく動く。
     */
    EA = 0;
    if (accept) {
        UEP2_CTRL = (uint8_t)((UEP2_CTRL & ~MASK_UEP_R_RES) | UEP_R_RES_ACK);
    } else {
        UEP2_CTRL = (uint8_t)((UEP2_CTRL & ~MASK_UEP_R_RES) | UEP_R_RES_NAK);
    }
    EA = saved_ea;
}

void usb_ep2_commit(uint8_t len)
{
    uint8_t saved_ea = EA;

    EA = 0;
    if ((UEP2_CTRL & MASK_UEP_T_RES) != UEP_T_RES_STALL) {
        ep2_busy = 1;
        UEP2_T_LEN = len;
        UEP2_CTRL = (uint8_t)((UEP2_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_ACK);
    }
    EA = saved_ea;
}

/* ------------------------------------------------------------------ */
/* クラスリクエスト                                                    */
/* ------------------------------------------------------------------ */
static uint8_t class_setup(void)
{
    uint8_t i;

    if (setup_iface == IF_HID) {
        switch (setup_req) {
        case HID_SET_IDLE:
            /* wValue 上位が idle rate。読み返しに合わせて覚えておく */
            hid_idle_rate = ep0_buf[3];
            return 0;
        case HID_SET_PROTOCOL:
            hid_protocol = ep0_buf[2];
            return 0;
        case HID_SET_REPORT:
            return 0;
        case HID_GET_REPORT: {
            /*
             * ダミーデバイスなので中身は無く 0 埋めで返す。ただし
             * Report ID を使っている以上、先頭バイトは要求された ID に
             * しておく。wValue の下位バイト (ep0_buf[2]) がその ID。
             *
             * 応答は同じ ep0_buf に書くので、SETUP パケットの中身は
             * ゼロ埋めで消える。ID は消す前に退避しておくこと。
             */
            uint8_t rid = ep0_buf[2];

            for (i = 0; i < EP1_SIZE; i++) {
                ep0_buf[i] = 0;
            }
            ep0_buf[0] = rid;
            return (setup_len > EP1_SIZE) ? EP1_SIZE : (uint8_t)setup_len;
        }
        case HID_GET_IDLE:
            ep0_buf[0] = hid_idle_rate;
            return 1;
        case HID_GET_PROTOCOL:
            ep0_buf[0] = hid_protocol;
            return 1;
        default:
            return STALL;
        }
    }

    if (setup_iface == IF_CDC_CTRL || setup_iface == IF_CDC_DATA) {
        switch (setup_req) {
        case CDC_SET_LINE_CODING:
            /* 実体は OUT ステージで届く */
            return 0;

        case CDC_GET_LINE_CODING:
            for (i = 0; i < 7; i++) {
                ep0_buf[i] = line_coding[i];
            }
            return (setup_len > 7) ? 7 : (uint8_t)setup_len;

        case CDC_SET_CTRL_LINE:
            control_line_state = ep0_buf[2];
#if CDC_DFU_1200BAUD
            /*
             * 1200 baud で開いて閉じる = DTR が落ちた瞬間を捕まえる。
             * Arduino Leonardo 以来おなじみの合図で、ホスト側は
             *   stty -f /dev/cu.usbmodemXXXX 1200
             * のように開いて閉じるだけでよい。
             * 1200 = 0x000004B0 をリトルエンディアンで比較している。
             *
             * ここで直接ブートローダへ飛ぶとステータスステージを
             * 返せないので、フラグだけ立ててメインループに任せる。
             */
            if ((control_line_state & CDC_DTR) == 0 &&
                line_coding[0] == 0xB0 && line_coding[1] == 0x04 &&
                line_coding[2] == 0x00 && line_coding[3] == 0x00) {
                dfu_pending = 1;
            }
#endif
            return 0;

        case CDC_SEND_BREAK:
            return 0;

        default:
            return STALL;
        }
    }

    return STALL;
}

/* ------------------------------------------------------------------ */
/* 制御転送                                                            */
/* ------------------------------------------------------------------ */
static uint8_t ep0_setup(void)
{
    uint8_t len = 0;

    if (USB_RX_LEN != 8) {
        return STALL;
    }

    setup_type = ep0_buf[0];
    setup_req = ep0_buf[1];
    setup_iface = ep0_buf[4]; /* wIndex 下位。インタフェース宛なら番号 */
    setup_len = (uint16_t)(((uint16_t)ep0_buf[7] << 8) | ep0_buf[6]);

    if ((setup_type & 0x60) != 0x00) {
        return class_setup();
    }

    switch (setup_req) {
    case REQ_GET_DESCRIPTOR:
        switch (ep0_buf[3]) {
        case 0x01: /* DEVICE */
            desc_ptr = dev_desc;
            len = dev_desc_len;
            break;
        case 0x02: /* CONFIGURATION */
            desc_ptr = cfg_desc;
            len = cfg_desc_len;
            break;
        case 0x03: /* STRING */
            switch (ep0_buf[2]) {
            case 0: desc_ptr = str_lang;    len = str_lang[0];    break;
            case 1: desc_ptr = str_vendor;  len = str_vendor[0];  break;
            case 2: desc_ptr = str_product; len = str_product[0]; break;
            case 3: {
                /*
                 * シリアルだけは実行時に組み立てた xdata なので、
                 * MOVC で読む copy_code は使えない。長さは
                 * 2 + 10*2 = 22 バイトで EP0_SIZE (64) に収まるから、
                 * 分割送信の経路にも乗せずここで返してしまう。
                 */
                uint8_t n = str_serial[0];
                uint8_t k;

                if (setup_len > n) {
                    setup_len = n;
                }
                n = (uint8_t)setup_len;
                for (k = 0; k < n; k++) {
                    ep0_buf[k] = str_serial[k];
                }
                /*
                 * 送り切ったので続きは無い。以降 IN が来ても
                 * ep0_in() が長さ 0 を返して終わる。
                 */
                setup_len = 0;
                return n;
            }
            default: return STALL;
            }
            break;
        case 0x22: /* HID Report */
            desc_ptr = hid_report_desc;
            len = hid_report_desc_len;
            break;
        default:
            return STALL;
        }
        /* wLength より長くは返さない */
        if (setup_len > len) {
            setup_len = len;
        }
        len = (setup_len >= EP0_SIZE) ? EP0_SIZE : (uint8_t)setup_len;
        copy_len = len;
        copy_desc_to_ep0();
        desc_ptr += len;
        setup_len -= len;
        break;

    case REQ_SET_ADDRESS:
        /*
         * アドレスはステータスステージが終わってから反映する。
         * ここで即座に書くと、そのトランザクションの ACK を
         * 新しいアドレスで返してしまい列挙に失敗する。
         */
        addr_pending = ep0_buf[2] & MASK_USB_ADDR;
        len = 0;
        break;

    case REQ_GET_CONFIGURATION:
        ep0_buf[0] = cfg_value;
        len = 1;
        break;

    case REQ_SET_CONFIGURATION:
        /* 持っている構成は 1 つだけ。0 (未構成に戻す) 以外は撥ねる */
        if (ep0_buf[2] > 1) {
            return STALL;
        }
        cfg_value = ep0_buf[2];
        ep_reset();
        len = 0;
        break;

    case REQ_GET_INTERFACE:
        /* 存在しないインタフェース番号は撥ねる。代替設定は 0 のみ */
        if (setup_iface > IF_HID) {
            return STALL;
        }
        ep0_buf[0] = 0;
        len = 1;
        break;

    case REQ_SET_INTERFACE:
        /*
         * 代替設定は 0 しか持たず、インタフェースは 3 本だけ。
         * wValue (代替設定) と wIndex (インタフェース番号) の両方を見る。
         */
        if (ep0_buf[2] != 0 || setup_iface > IF_HID) {
            return STALL;
        }
        ep_reset_iface(setup_iface);
        len = 0;
        break;

    case REQ_GET_STATUS:
        ep0_buf[0] = 0x00;
        ep0_buf[1] = 0x00;
        switch (setup_type & 0x1F) {
        case 0x00: /* デバイス: bit0 セルフパワー, bit1 リモートウェイクアップ */
            if (remote_wakeup) {
                ep0_buf[0] = 0x02;
            }
            break;
        case 0x01: /* インタフェース: 予約なので 0 */
            break;
        case 0x02: { /* 端点: bit0 がハルト */
            uint8_t h;

            /* EP0 も有効な宛先。ハルトしないので常に 0 を返す */
            if ((ep0_buf[4] & 0x7F) == 0x00) {
                break;
            }
            h = ep_halted(ep0_buf[4]);
            if (h == 0xFF) {
                return STALL;
            }
            /*
             * ここを常に 0 で返していると、SET_FEATURE(ENDPOINT_HALT)
             * で本当に STALL しているのに「ハルトしていない」と
             * 答えることになる。状態を見てから CLEAR_FEATURE を出す
             * ホストは「消すものが無い」と判断して何もしないので、
             * 端点は挿し直すまで止まったままになる。
             */
            ep0_buf[0] = h;
            break;
        }
        default:
            return STALL;
        }
        len = 2;
        break;

    /*
     * 端点あての CLEAR_FEATURE / SET_FEATURE は、名指しされた向きの
     * ビットだけを触ること。UEP2_CTRL を丸ごと書き直すと、IN の
     * ハルトを消したつもりで OUT のトグルまで DATA0 に落ちる。
     * EP2 の IN と OUT は別々の端点で、トグルも別々に進む。
     * ホスト側だけ DATA1 のままになるので、次の OUT パケットは
     * U_TOG_OK が立たずに黙って捨てられ、しかも SIE は ACK を
     * 返しているのでホストは再送しない。コンソールに打った 1 行が
     * 消える、という形で出る。
     */
    case REQ_CLEAR_FEATURE:
        switch (setup_type & 0x1F) {
        case 0x00: /* デバイスあて */
            if (ep0_buf[2] != FEATURE_DEVICE_REMOTE_WAKEUP) {
                return STALL;
            }
            remote_wakeup = 0;
            break;
        case 0x02: /* 端点あて */
            if (ep0_buf[2] != FEATURE_ENDPOINT_HALT) {
                return STALL;
            }
            /* EP0 は有効な宛先だが、ハルトしないので何もせず成功 */
            if ((ep0_buf[4] & 0x7F) == 0x00) {
                break;
            }
            switch (ep0_buf[4]) {
            case 0x81:
                UEP1_CTRL = (uint8_t)((UEP1_CTRL &
                                       ~(MASK_UEP_T_RES | bUEP_T_TOG)) |
                                      UEP_T_RES_NAK);
                ep1_busy = 0;
                break;
            case 0x82:
                UEP2_CTRL = (uint8_t)((UEP2_CTRL &
                                       ~(MASK_UEP_T_RES | bUEP_T_TOG)) |
                                      UEP_T_RES_NAK);
                ep2_busy = 0;
                break;
            case 0x02:
                /*
                 * トグルを落として応答を戻すが、ACK にしてよいかは
                 * 受信リングの空き次第。無条件に ACK にすると、
                 * 空きが 1 パケット未満のところに 64 バイト受け取って
                 * 落としながらホストには成功を返すことになる。
                 */
                UEP2_CTRL = (uint8_t)((UEP2_CTRL &
                                       ~(MASK_UEP_R_RES | bUEP_R_TOG)) |
                                      UEP_R_RES_NAK);
                cdc_rx_rearm();
                break;
            case 0x83:
                UEP3_CTRL = (uint8_t)((UEP3_CTRL &
                                       ~(MASK_UEP_T_RES | bUEP_T_TOG)) |
                                      UEP_T_RES_NAK);
                break;
            default:
                return STALL;
            }
            break;
        default:
            /* インタフェースあての機能は定義されていない */
            return STALL;
        }
        len = 0;
        break;

    case REQ_SET_FEATURE:
        switch (setup_type & 0x1F) {
        case 0x00: /* デバイスあて */
            if (ep0_buf[2] != FEATURE_DEVICE_REMOTE_WAKEUP) {
                return STALL;
            }
#if SUSPEND_POLICY == SUSPEND_WAKE
            remote_wakeup = 1;
#else
            /*
             * bmAttributes の bit5 を立てていない構成では、
             * この要求は Request Error を返す決まり (9.4.9)。
             * 受理してしまうと GET_STATUS が「起こせます」と
             * 答えるのに実際は起こせない、という矛盾になる。
             */
            return STALL;
#endif
            break;
        case 0x02: /* 端点あて */
            if (ep0_buf[2] != FEATURE_ENDPOINT_HALT) {
                return STALL;
            }
            switch (ep0_buf[4]) {
            case 0x81:
                UEP1_CTRL = (uint8_t)((UEP1_CTRL & ~MASK_UEP_T_RES) |
                                      UEP_T_RES_STALL);
                break;
            case 0x82:
                UEP2_CTRL = (uint8_t)((UEP2_CTRL & ~MASK_UEP_T_RES) |
                                      UEP_T_RES_STALL);
                break;
            case 0x02:
                /* CLEAR_FEATURE が受けるのに SET_FEATURE が撥ねるのは片手落ち */
                UEP2_CTRL = (uint8_t)((UEP2_CTRL & ~MASK_UEP_R_RES) |
                                      UEP_R_RES_STALL);
                break;
            case 0x83:
                UEP3_CTRL = (uint8_t)((UEP3_CTRL & ~MASK_UEP_T_RES) |
                                      UEP_T_RES_STALL);
                break;
            default:
                return STALL;
            }
            break;
        default:
            return STALL;
        }
        len = 0;
        break;

    default:
        return STALL;
    }

    return len;
}

static void ep0_in(void)
{
    uint8_t len;

    /*
     * setup_req だけで分岐すると、クラス要求の番号がたまたま
     * 標準要求と重なったときに別経路へ迷い込む (HID には無いが、
     * 将来 0x05 / 0x06 のクラス要求を足せば起きる)。
     * 標準要求のときだけこの分岐に入る。
     */
    if ((setup_type & 0x60) != 0x00) {
        UEP0_T_LEN = 0;
        UEP0_CTRL = bUEP_R_TOG | UEP_R_RES_ACK | UEP_T_RES_NAK;
        return;
    }

    switch (setup_req) {
    case REQ_GET_DESCRIPTOR:
        len = (setup_len >= EP0_SIZE) ? EP0_SIZE : (uint8_t)setup_len;
        copy_len = len;
        copy_desc_to_ep0();
        desc_ptr += len;
        setup_len -= len;
        UEP0_T_LEN = len;
        UEP0_CTRL ^= bUEP_T_TOG; /* EP0 は自動トグルが無い */
        break;

    case REQ_SET_ADDRESS:
        USB_DEV_AD = (uint8_t)((USB_DEV_AD & 0x80) | addr_pending);
        UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
        break;

    default:
        /*
         * データステージ終わり。続くステータスステージ OUT は
         * DATA1 で来るので、受信側のトグルも立てておく。
         */
        UEP0_T_LEN = 0;
        UEP0_CTRL = bUEP_R_TOG | UEP_R_RES_ACK | UEP_T_RES_NAK;
        break;
    }
}

static void ep0_out(void)
{
    uint8_t i;
    uint8_t n = USB_RX_LEN;

    if ((setup_type & 0x60) != 0x00) {
        if (setup_iface == IF_HID && setup_req == HID_SET_REPORT) {
            /*
             * キーボード LED の状態。Report ID を使っているので
             * 先頭バイトが ID、その次が LED ビットになる。
             */
            if (n >= 2 && ep0_buf[0] == REPORT_ID_KEYBOARD) {
                led_report = ep0_buf[1];
            } else if (n >= 1) {
                led_report = ep0_buf[0];
            }
        } else if ((setup_iface == IF_CDC_CTRL ||
                    setup_iface == IF_CDC_DATA) &&
                   setup_req == CDC_SET_LINE_CODING) {
            if (n > 7) {
                n = 7;
            }
            for (i = 0; i < n; i++) {
                line_coding[i] = ep0_buf[i];
            }
        }
    }
    /*
     * データステージ付きの制御転送 (SET_LINE_CODING など) では、
     * このあとのステータスステージ IN は DATA1 でなければならない。
     * ここで bUEP_T_TOG を落とすと DATA0 で返してしまう。
     * EP0 には自動トグルが無いので明示的に立てておく。
     */
    UEP0_T_LEN = 0;
    UEP0_CTRL = bUEP_R_TOG | bUEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_ACK;
}

/* ------------------------------------------------------------------ */
/* 割り込み                                                            */
/* ------------------------------------------------------------------ */
void usb_isr(void) __interrupt(INT_NO_USB)
{
    uint8_t len;

    if (UIF_TRANSFER) {
        switch (USB_INT_ST & (MASK_UIS_TOKEN | MASK_UIS_ENDP)) {
        case UIS_TOKEN_IN | 1:
            /* HID レポートを送り終えた */
            UEP1_T_LEN = 0;
            UEP1_CTRL = (uint8_t)((UEP1_CTRL & ~MASK_UEP_T_RES) |
                                  UEP_T_RES_NAK);
            ep1_busy = 0;
            break;

        case UIS_TOKEN_IN | 2:
            /* CDC の送信が済んだ */
            UEP2_T_LEN = 0;
            UEP2_CTRL = (uint8_t)((UEP2_CTRL & ~MASK_UEP_T_RES) |
                                  UEP_T_RES_NAK);
            ep2_busy = 0;
            break;

        case UIS_TOKEN_OUT | 2:
            /* CDC でホストから届いた。トグルが合っている時だけ拾う */
            if (U_TOG_OK) {
                cdc_on_rx(ep2_buf, USB_RX_LEN);
            }
            break;

        case UIS_TOKEN_IN | 3:
            /* 通知端点。何も送っていないので NAK に戻すだけ */
            UEP3_T_LEN = 0;
            UEP3_CTRL = (uint8_t)((UEP3_CTRL & ~MASK_UEP_T_RES) |
                                  UEP_T_RES_NAK);
            break;

        case UIS_TOKEN_SETUP | 0:
            len = ep0_setup();
            if (len == STALL) {
                UEP0_T_LEN = 0;
                UEP0_CTRL = bUEP_R_TOG | bUEP_T_TOG |
                            UEP_R_RES_STALL | UEP_T_RES_STALL;
            } else {
                UEP0_T_LEN = len;
                /* データステージ (または状態ステージ) は DATA1 から */
                UEP0_CTRL = bUEP_R_TOG | bUEP_T_TOG |
                            UEP_R_RES_ACK | UEP_T_RES_ACK;
            }
            break;

        case UIS_TOKEN_IN | 0:
            ep0_in();
            break;

        case UIS_TOKEN_OUT | 0:
            ep0_out();
            break;

        default:
            break;
        }
        UIF_TRANSFER = 0;
    }

    if (UIF_BUS_RST) {
        UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
        /*
         * 端点の初期化は ep_reset() に任せる。ここで UEPn_CTRL を
         * 直書きしていたので、EP2 OUT を空きも見ずに ACK に戻していて、
         * SET_CONFIGURATION 側 (ep2_reset) だけが正しいという
         * 二重管理になっていた。同じ初期化が 2 箇所にあると、
         * 次に直すとき片方だけ直る。
         */
        ep_reset();
        USB_DEV_AD = 0x00;
        cfg_value = 0;
        suspended = 0;
        control_line_state = 0;
        remote_wakeup = 0;

        /*
         * クラス側の状態も既定に戻す。HID の protocol は規格上
         * リセットで Report に戻る。line_coding を戻しておくと
         * 「前の open が 1200 のまま残っていて、次に DTR が落ちた
         * 瞬間に DFU へ飛ぶ」という筋も消える。
         */
        hid_protocol = 1;
        hid_idle_rate = 0;
        line_coding[0] = 0x00;
        line_coding[1] = 0xC2;
        line_coding[2] = 0x01;
        line_coding[3] = 0x00;
        line_coding[4] = 0x00;
        line_coding[5] = 0x00;
        line_coding[6] = 0x08;
        /*
         * 自分のフラグだけ落とす。0xFF を書くと直後に見る
         * UIF_SUSPEND まで消えてしまい、リセットと同時に
         * サスペンドが立った場合に取りこぼす。サスペンドは
         * エッジなので二度と上がらず、バスが止まっているのに
         * 眠らないまま mA を流し続けることになる。
         */
        UIF_BUS_RST = 0;
    }

    if (UIF_SUSPEND) {
        UIF_SUSPEND = 0;
        suspended = (uint8_t)((USB_MIS_ST & bUMS_SUSPEND) ? 1 : 0);
    }
}
