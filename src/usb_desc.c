/* SPDX-License-Identifier: MIT */
#include "ch552.h"
#include "config.h"
#include "usb_desc.h"

/* ------------------------------------------------------------------ */
/* デバイス記述子                                                      */
/* ------------------------------------------------------------------ */
__code const uint8_t dev_desc[] = {
    18,                     /* bLength                                  */
    0x01,                   /* bDescriptorType = DEVICE                 */
    /*
     * bcdUSB は 2.00 にしておくこと。IAD (Interface Association
     * Descriptor) を使うデバイスは 0xEF/0x02/0x01 を名乗るだけでなく
     * bcdUSB >= 0x0200 であることが ECN で要求されている。
     * Windows の usbccgp はここを見ていて、1.10 のままだと IAD を
     * 無視してインタフェース単位で分解する。すると CDC 制御だけが
     * 相方のデータインタフェース無しで残り、usbser.sys が結び付かず
     * COM ポートが出ない。macOS と Linux は IAD を読むので通る。
     */
    0x00, 0x02,             /* bcdUSB = 2.00 (IAD を使うので必須)       */
    /*
     * IAD を含む複合デバイスは 0xEF/0x02/0x01 (Miscellaneous Device /
     * Common Class / Interface Association) と名乗る決まりになっている。
     * ここを 0x00 のままにすると、ホストによっては IAD を読まずに
     * CDC の制御とデータを結び付けられず、シリアルポートが出ない。
     */
    0xEF,                   /* bDeviceClass                             */
    0x02,                   /* bDeviceSubClass                          */
    0x01,                   /* bDeviceProtocol                          */
    EP0_SIZE,               /* bMaxPacketSize0                          */
    (uint8_t)(USB_VID & 0xFF), (uint8_t)(USB_VID >> 8),
    (uint8_t)(USB_PID & 0xFF), (uint8_t)(USB_PID >> 8),
    0x00, 0x01,             /* bcdDevice = 1.00                         */
    0x01,                   /* iManufacturer                            */
    0x02,                   /* iProduct                                 */
    0x03,                   /* iSerialNumber                            */
    0x01                    /* bNumConfigurations                       */
};
__code const uint8_t dev_desc_len = sizeof(dev_desc);

/* ------------------------------------------------------------------ */
/* HID レポート記述子                                                  */
/* ------------------------------------------------------------------ */
__code const uint8_t hid_report_desc[] = {
    /* --- Report ID 1: キーボード ---------------------------------- */
    0x05, 0x01,             /* Usage Page (Generic Desktop)             */
    0x09, 0x06,             /* Usage (Keyboard)                         */
    0xA1, 0x01,             /* Collection (Application)                 */
    0x85, REPORT_ID_KEYBOARD,
    0x05, 0x07,             /*   Usage Page (Keyboard/Keypad)           */
    0x19, 0xE0,             /*   Usage Minimum (LeftControl)            */
    0x29, 0xE7,             /*   Usage Maximum (Right GUI)              */
    0x15, 0x00, 0x25, 0x01, /*   Logical 0..1                           */
    0x75, 0x01, 0x95, 0x08, /*   1bit x 8                               */
    0x81, 0x02,             /*   Input (Data,Var,Abs) = 修飾キー        */
    0x95, 0x01, 0x75, 0x08,
    0x81, 0x03,             /*   Input (Const) = 予約バイト             */
    0x95, 0x06, 0x75, 0x08, /*   8bit x 6                               */
    0x15, 0x00, 0x25, 0x65, /*   Logical 0..101                         */
    0x05, 0x07,
    0x19, 0x00, 0x29, 0x65,
    0x81, 0x00,             /*   Input (Data,Array) = キーコード 6 個   */
    /*
     * LED 出力。OUT 端点は用意していないので、ホストは制御転送の
     * SET_REPORT で送ってくる。今は使わないが、CapsLock の点滅を
     * 合図に使うといった小細工の余地を残しておく。
     */
    0x95, 0x05, 0x75, 0x01,
    0x05, 0x08,             /*   Usage Page (LEDs)                      */
    0x19, 0x01, 0x29, 0x05,
    /*
     * 論理範囲を 0..1 に引き直す。ここを書かないと、直前の
     * キーコード配列の Logical Maximum 101 を引き継いだまま
     * Report Size 1 の項目になり、「1bit に 101 が入る」という
     * 矛盾した記述子になる。寛容なホストは通すが、厳格な
     * パーサは記述子ごと撥ねるので HID が丸ごと出なくなる。
     */
    0x15, 0x00, 0x25, 0x01, /*   Logical Minimum 0 / Maximum 1          */
    0x91, 0x02,             /*   Output (Data,Var,Abs)                  */
    0x95, 0x01, 0x75, 0x03,
    0x91, 0x03,             /*   Output (Const) = 詰め物                */
    0xC0,                   /* End Collection                           */

    /* --- Report ID 2: マウス -------------------------------------- */
    0x05, 0x01,             /* Usage Page (Generic Desktop)             */
    0x09, 0x02,             /* Usage (Mouse)                            */
    0xA1, 0x01,             /* Collection (Application)                 */
    0x85, REPORT_ID_MOUSE,
    0x09, 0x01,             /*   Usage (Pointer)                        */
    0xA1, 0x00,             /*   Collection (Physical)                  */
    0x05, 0x09,             /*     Usage Page (Button)                  */
    0x19, 0x01, 0x29, 0x03, /*     Button 1..3                          */
    0x15, 0x00, 0x25, 0x01,
    0x95, 0x03, 0x75, 0x01,
    0x81, 0x02,             /*     Input (Data,Var,Abs) = ボタン        */
    0x95, 0x01, 0x75, 0x05,
    0x81, 0x03,             /*     Input (Const) = 詰め物               */
    0x05, 0x01,
    0x09, 0x30,             /*     Usage (X)                            */
    0x09, 0x31,             /*     Usage (Y)                            */
    0x09, 0x38,             /*     Usage (Wheel)                        */
    0x15, 0x81, 0x25, 0x7F, /*     Logical -127..127                    */
    0x75, 0x08, 0x95, 0x03,
    0x81, 0x06,             /*     Input (Data,Var,Rel) = 相対移動      */
    0xC0,                   /*   End Collection                         */
    0xC0                    /* End Collection                           */
};
__code const uint8_t hid_report_desc_len = sizeof(hid_report_desc);

/* ------------------------------------------------------------------ */
/* コンフィギュレーション記述子                                        */
/* ------------------------------------------------------------------ */
/*   config 9 + IAD 8 + IF0 9 + 機能記述子 19 + EP3 7
 *   + IF1 9 + EP2 IN 7 + EP2 OUT 7 + IF2 9 + HID 9 + EP1 7 = 100
 */
#define CFG_TOTAL_LEN 100

__code const uint8_t cfg_desc[] = {
    /* --- Configuration -------------------------------------------- */
    9, 0x02,
    (uint8_t)(CFG_TOTAL_LEN & 0xFF), (uint8_t)(CFG_TOTAL_LEN >> 8),
    0x03,                   /* bNumInterfaces = CDC 制御 / CDC データ / HID */
    0x01,                   /* bConfigurationValue                      */
    0x00,                   /* iConfiguration                           */
    /*
     * bmAttributes。bit7 は必ず 1、bit5 がリモートウェイクアップ対応。
     * ここを立てていないと、ホストは SET_FEATURE(DEVICE_REMOTE_WAKEUP)
     * を送ってこないので、デバイスから起こすことは一切できない。
     */
#if SUSPEND_POLICY == SUSPEND_WAKE
    0x80 | 0x20,            /* バスパワー + リモートウェイクアップ対応   */
#else
    0x80,                   /* バスパワー                               */
#endif
    USB_MAX_POWER_MA / 2,   /* bMaxPower は 2mA 単位                    */

    /* --- IAD: IF0 と IF1 を CDC として束ねる ---------------------- */
    8, 0x0B,
    IF_CDC_CTRL,            /* bFirstInterface                          */
    0x02,                   /* bInterfaceCount                          */
    0x02,                   /* bFunctionClass = CDC                     */
    0x02,                   /* bFunctionSubClass = ACM                  */
    0x01,                   /* bFunctionProtocol = AT commands          */
    0x00,                   /* iFunction                                */

    /* --- IF0: CDC Communications ---------------------------------- */
    9, 0x04,
    IF_CDC_CTRL, 0x00,
    0x01,                   /* bNumEndpoints = 通知用の 1 本            */
    0x02,                   /* bInterfaceClass = CDC                    */
    0x02,                   /* bInterfaceSubClass = ACM                 */
    0x01,                   /* bInterfaceProtocol = AT commands         */
    0x00,

    /* CDC Header 機能記述子 */
    5, 0x24, 0x00, 0x10, 0x01,   /* bcdCDC = 1.10                       */
    /* Call Management 機能記述子 */
    5, 0x24, 0x01,
    0x00,                        /* bmCapabilities: 呼制御はしない      */
    IF_CDC_DATA,                 /* bDataInterface                      */
    /* ACM 機能記述子 */
    4, 0x24, 0x02,
    0x02,                        /* Set/Get_Line_Coding などに対応      */
    /* Union 機能記述子 */
    5, 0x24, 0x06,
    IF_CDC_CTRL,                 /* bControlInterface                   */
    IF_CDC_DATA,                 /* bSubordinateInterface0              */

    /* EP3 IN: 通知。実際には何も送っていないが、ACM の作法として要る */
    7, 0x05,
    0x83, 0x03,
    EP3_SIZE, 0x00,
    0xFF,                   /* bInterval。通知は使わないので最長で良い  */

    /* --- IF1: CDC Data -------------------------------------------- */
    9, 0x04,
    IF_CDC_DATA, 0x00,
    0x02,                   /* bNumEndpoints = IN と OUT                */
    0x0A,                   /* bInterfaceClass = CDC Data               */
    0x00, 0x00, 0x00,

    /* EP2 OUT (ホスト -> デバイス) */
    7, 0x05,
    0x02, 0x02,
    EP2_SIZE, 0x00,
    0x00,

    /* EP2 IN (デバイス -> ホスト) */
    7, 0x05,
    0x82, 0x02,
    EP2_SIZE, 0x00,
    0x00,

    /* --- IF2: HID ------------------------------------------------- */
    9, 0x04,
    IF_HID, 0x00,
    0x01,                   /* bNumEndpoints                            */
    0x03,                   /* bInterfaceClass = HID                    */
    /*
     * Boot サブクラスにはしない。Report ID を使う時点で Boot
     * プロトコルとしては解釈できないので、素直に 0/0 にしておく。
     */
    0x00, 0x00, 0x00,

    /* HID 記述子 */
    9, 0x21,
    0x11, 0x01,             /* bcdHID = 1.11                            */
    0x00,                   /* bCountryCode                             */
    0x01,                   /* bNumDescriptors                          */
    0x22,                   /* bDescriptorType = Report                 */
    (uint8_t)(sizeof(hid_report_desc) & 0xFF),
    (uint8_t)(sizeof(hid_report_desc) >> 8),

    /* EP1 IN: HID レポート */
    7, 0x05,
    0x81, 0x03,
    EP1_SIZE, 0x00,
    USB_HID_INTERVAL_MS
};
__code const uint8_t cfg_desc_len = sizeof(cfg_desc);

/*
 * wTotalLength は手で数えた値なので、実体とずれたらビルドを止める。
 * ここがずれると「列挙はするのに端点が 1 つ足りない」といった
 * 見つけにくい壊れ方をする。
 */
typedef char cfg_len_check[(sizeof(cfg_desc) == CFG_TOTAL_LEN) ? 1 : -1];

/* ------------------------------------------------------------------ */
/* 文字列記述子 (UTF-16LE)                                             */
/* ------------------------------------------------------------------ */
__code const uint8_t str_lang[] = {
    4, 0x03, 0x09, 0x04     /* 0x0409 = English (United States)         */
};

__code const uint8_t str_vendor[] = {
    12, 0x03,
    'T', 0, 'a', 0, 'd', 0, 'a', 0, 'k', 0
};

__code const uint8_t str_product[] = {
    24, 0x03,
    'C', 0, 'H', 0, '5', 0, '5', 0, '2', 0, ' ', 0,
    'D', 0, 'u', 0, 'm', 0, 'm', 0, 'y', 0
};

/* ------------------------------------------------------------------ */
/* シリアル番号 (実行時にチップ固有 ID から生成)                       */
/* ------------------------------------------------------------------ */
/*
 * 固定文字列だと同じ個体を複数挿したときにホストが区別できない。
 * macOS は VID/PID とシリアル番号の組で個体を識別するので、
 * 全部同じだと /dev/cu.usbmodem* の名前が衝突したり、片方の設定が
 * もう片方に効いたりする。
 *
 * 記述子は「自分の長さを先頭に持つ」形式なので、UTF-16LE で
 * 10 文字なら bLength は 2 + 10*2 = 22。
 */
__xdata uint8_t str_serial[2 + SERIAL_CHARS * 2];

/*
 * 読み出したチップ ID。id_bytes[0] が最下位、id_bytes[4] が最上位。
 * データシートの並びは
 *   3FFA = 最上位バイト   (3FFB は予約)
 *   3FFC = 最下位, 3FFD = 下から 2 番目
 *   3FFE = 上から 2 番目, 3FFF = 上から 1 番目 (40bit のうちの 32bit 側)
 * なので、そのまま昇順に詰め替えている。
 */
static __xdata uint8_t id_bytes[SERIAL_BYTES];

/* ID が読めなかったときの値。桁数を変えないよう最下位だけ 1 にする */
static void serial_fallback(void)
{
    uint8_t i;

    for (i = 0; i < SERIAL_BYTES; i++) {
        id_bytes[i] = 0x00;
    }
    id_bytes[0] = 0x01;
}

static uint8_t hex_digit(uint8_t v)
{
    v &= 0x0F;
    return (uint8_t)(v < 10 ? ('0' + v) : ('A' + (v - 10)));
}

void usb_desc_init_serial(void)
{
    uint8_t i;

#if SERIAL_FROM_CHIP_ID
    /*
     * フラッシュを読む間は割り込みを止める。EA ではなく E_DIS を
     * 使うのは、こちらが「フラッシュ操作中の割り込み禁止」用に
     * 用意されているビットだから (WCH のサンプルも同じ)。
     *
     * 読み出し先の id_bytes[] を xdata の static にしてあるのは、
     * ローカル配列にすると SDCC が iRAM のオーバレイ領域を要求して、
     * ここでは足りずにリンクが通らないため。
     */
    E_DIS = 1;
    id_bytes[0] = *(__code const uint8_t *)(ROM_CHIP_ID_LO + 0);
    id_bytes[1] = *(__code const uint8_t *)(ROM_CHIP_ID_LO + 1);
    id_bytes[2] = *(__code const uint8_t *)(ROM_CHIP_ID_HI + 0);
    id_bytes[3] = *(__code const uint8_t *)(ROM_CHIP_ID_HI + 1);
    id_bytes[4] = *(__code const uint8_t *)(ROM_CHIP_ID_HX);
    E_DIS = 0;

    /*
     * 消去済みフラッシュ (全部 0xFF) や読めなかった場合 (全部 0x00) は
     * ID として信用せず固定値に落とす。実機で 0000000001 が見えたら
     * この領域が MOVC で読めていないということ。
     */
    {
        uint8_t all_and = 0xFF;
        uint8_t all_or = 0x00;

        for (i = 0; i < SERIAL_BYTES; i++) {
            all_and &= id_bytes[i];
            all_or |= id_bytes[i];
        }
        if (all_and == 0xFF || all_or == 0x00) {
            serial_fallback();
        }
    }
#else
    serial_fallback();
#endif

    str_serial[0] = 2 + SERIAL_CHARS * 2;
    str_serial[1] = 0x03;
    for (i = 0; i < SERIAL_CHARS; i++) {
        /*
         * 上位バイトから、各バイトを上位ニブル -> 下位ニブルの順に。
         * i=0 が id_bytes[4] の上位 4bit なので、表示は
         * 「ID を 40bit の数と見た 16 進 10 桁」と一致する。
         */
        uint8_t byte = id_bytes[(SERIAL_BYTES - 1) - (i >> 1)];
        uint8_t nib = (i & 1) ? byte : (uint8_t)(byte >> 4);

        str_serial[2 + i * 2] = hex_digit(nib);
        str_serial[3 + i * 2] = 0x00; /* UTF-16LE の上位バイト */
    }
}
