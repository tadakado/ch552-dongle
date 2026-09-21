/* SPDX-License-Identifier: MIT */
/*
 * ch552.h - CH551/CH552 特殊機能レジスタ定義
 *
 * WCH CH552 データシート (CH552DS1) の SFR 一覧表から起こしたもの。
 * レジスタのアドレスは事実であって著作物ではないため、ここでは
 * ベンダ提供ヘッダを流用せず、必要なものだけを自前で定義している。
 *
 * 対象: CH551 / CH552 (8051 互換コア、命令の 79% が 1 クロック)
 * 注意: SDCC 専用構文 (__sfr / __sbit / __at) を使う。Keil では通らない。
 */
#ifndef CH552_H
#define CH552_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* コア (標準 8051)                                                    */
/* ------------------------------------------------------------------ */
__sfr __at(0x81) SP;
__sfr __at(0x82) DPL;
__sfr __at(0x83) DPH;
__sfr __at(0x87) PCON;
__sfr __at(0xD0) PSW;
__sfr __at(0xE0) ACC;
__sfr __at(0xF0) B;

/* ------------------------------------------------------------------ */
/* システム設定・クロック                                              */
/* ------------------------------------------------------------------ */
__sfr __at(0xA1) SAFE_MOD;    /* 書込専用: 0x55,0xAA でセーフモード解錠 */
__sfr __at(0xA1) CHIP_ID;     /* 読出時は同アドレスがチップ ID          */
__sfr __at(0xA2) XBUS_AUX;
__sfr __at(0xA9) WAKE_CTRL;   /* セーフモード解錠中のみ書込可           */
__sfr __at(0xB1) GLOBAL_CFG;  /* セーフモード解錠中のみ書込可           */
__sfr __at(0xB9) CLOCK_CFG;   /* 同上                                   */
__sfr __at(0xFE) RESET_KEEP;
__sfr __at(0xFF) WDOG_COUNT;

/* GLOBAL_CFG */
#define bBOOT_LOAD  0x20      /* 読出専用: ブートローダで起動したか      */
#define bSW_RESET   0x10      /* ソフトウェアリセット (自動クリア)       */
#define bCODE_WE    0x08      /* コードフラッシュ書換許可                */
#define bDATA_WE    0x04      /* データフラッシュ書換許可                */
#define bLDO3V3_OFF 0x02
#define bWDOG_EN    0x01

/* CLOCK_CFG 下位 3bit = システムクロック分周選択                       */
#define MASK_SYS_CK_SEL 0x07
#define SYS_CK_187K5    0x00
#define SYS_CK_750K     0x01
#define SYS_CK_3M       0x02
#define SYS_CK_6M       0x03  /* リセット直後の既定値                    */
#define SYS_CK_12M      0x04
#define SYS_CK_16M      0x05  /* VCC 3.3V 超が条件                       */
#define SYS_CK_24M      0x06  /* VCC 4.4V 超が条件 (VBUS 直結なら可)     */
#define bOSC_EN_INT     0x80
#define bOSC_EN_XT      0x40
#define bWDOG_IF_TO     0x20
/* 基準は Fosc (発振器) であって Fsys ではない。内蔵発振なら常に 24MHz */
#define bROM_CLK_FAST   0x10  /* 0 = Fosc >= 16MHz 用, 1 = Fosc < 16MHz 用 */

/* PCON */
#define PD              0x02  /* スリープ。起床時にハードが自動でクリア  */
/*
 * bit0 は CH552 では予約 (読み出し専用)。8051 の IDL に相当する
 * アイドルモードは無い。WCH の公式ヘッダにも定義されていないので、
 * ここでも定義しない。書いても何も起きないビットを置いておくと、
 * いつか PCON |= IDL と書いて「省電力にしたつもり」になる。
 */

/*
 * WAKE_CTRL: スリープからの起床要因。
 *   bit7 USB のバス活動
 *   bit6 RXD1 の Low
 *   bit5 P1.5 の Low
 *   bit4 P1.4 の Low
 *   bit3 P1.3 の Low
 *   bit2 RST の **High**
 *   bit1 P3.2 の **エッジ変化** と P3.3 の Low
 *   bit0 RXD0 の Low
 * P1.7 (タッチパッド) はこの表に無い。**スリープ中にタッチで
 * 起こすことはハード的に無理**、というのがここで押さえたい点。
 * (データシートの特徴一覧には SPI0 も起床要因として挙がっているが、
 *  対応するビットは WAKE_CTRL に無い。)
 */
#define bWAK_BY_USB     0x80
#define bWAK_RXD1_LO    0x40
#define bWAK_P1_5_LO    0x20
#define bWAK_P1_4_LO    0x10
#define bWAK_P1_3_LO    0x08
#define bWAK_RST_HI     0x04
#define bWAK_P3_2E_3L   0x02
#define bWAK_RXD0_LO    0x01

/* ------------------------------------------------------------------ */
/* Flash-ROM / Data-Flash                                              */
/* ------------------------------------------------------------------ */
__sfr __at(0x84) ROM_ADDR_L;
__sfr __at(0x85) ROM_ADDR_H;
__sfr __at(0x86) ROM_CTRL;    /* 書込時: コマンド                       */
__sfr __at(0x86) ROM_STATUS;  /* 読出時: 状態 (同一アドレス)            */
__sfr __at(0x8E) ROM_DATA_L;
__sfr __at(0x8F) ROM_DATA_H;

#define ROM_CMD_WRITE   0x9A
#define ROM_CMD_READ    0x8E
#define bROM_ADDR_OK    0x40  /* 読出専用: アドレスが有効だった          */
#define bROM_CMD_ERR    0x02  /* 読出専用: 未知のコマンド                */

/*
 * Data-Flash は 128 バイト。アドレス空間上は 0xC000-0xC0FF に並ぶが
 * 「偶数アドレスのみ有効」なので、バイト添字 i は 0xC000 + (i << 1)。
 */
#define DATA_FLASH_ADDR 0xC000
#define DATA_FLASH_SIZE 128

#define BOOT_LOAD_ADDR  0x3800  /* ブートローダ先頭。ユーザ領域の直後    */

/*
 * チップ固有 ID。コード空間の末尾、0x3FF8-0x3FFF の「設定情報領域」に
 * 置かれている。ブートローダ本体 (0x3800-0x3FF7) とは別の領域で、
 * ユーザ領域 (0x3800 未満) の外。--code-size はリンカの割り当て検査に
 * 効くだけなので、MOVC 自体は 64KB 全域に届く。
 * データシート 6.7 が "This ID can be obtained by reading the Code Flash"、
 * 6.5 の 4 が "Read flash-ROM: Directly use MOVC command" と書いていて、
 * 読めることは明記されている。
 *
 *   0x3FFC-0x3FFD  ID 下位ワード (リトルエンディアン)
 *   0x3FFE-0x3FFF  ID 上位ワード
 *   0x3FFA         最上位バイト (下位 8bit のみ有効)
 *
 * 読む前後で E_DIS を立てる。WCH のサンプルもそうしている。
 */
#define ROM_CFG_ADDR      0x3FF8
#define ROM_CHIP_ID_HX    0x3FFA
#define ROM_CHIP_ID_LO    0x3FFC
#define ROM_CHIP_ID_HI    0x3FFE

/* ------------------------------------------------------------------ */
/* 割り込み                                                            */
/* ------------------------------------------------------------------ */
__sfr __at(0xA8) IE;
__sbit __at(0xA8 + 7) EA;
__sbit __at(0xA8 + 6) E_DIS;  /* 1 で割り込みを強制禁止。フラッシュ読出中に使う */
__sbit __at(0xA8 + 5) ET2;
__sbit __at(0xA8 + 4) ES;
__sbit __at(0xA8 + 3) ET1;
__sbit __at(0xA8 + 2) EX1;
__sbit __at(0xA8 + 1) ET0;
__sbit __at(0xA8 + 0) EX0;

__sfr __at(0xB8) IP;
__sfr __at(0xE8) IE_EX;
__sbit __at(0xE8 + 2) IE_USB;
__sbit __at(0xE8 + 1) IE_TKEY;
__sfr __at(0xE9) IP_EX;
__sfr __at(0xC7) GPIO_IE;

/* 割り込みベクタ番号 */
#define INT_NO_INT0  0
#define INT_NO_TMR0  1
#define INT_NO_INT1  2
#define INT_NO_TMR1  3
#define INT_NO_UART0 4
#define INT_NO_TMR2  5
#define INT_NO_SPI0  6
#define INT_NO_TKEY  7
#define INT_NO_USB   8
#define INT_NO_ADC   9
#define INT_NO_UART1 10
#define INT_NO_PWMX  11
#define INT_NO_GPIO  12
#define INT_NO_WDOG  13

/* ------------------------------------------------------------------ */
/* ポート                                                              */
/* ------------------------------------------------------------------ */
__sfr __at(0x90) P1;
__sbit __at(0x90 + 0) P1_0;   /* TIN0 / T2 / CAP1                       */
__sbit __at(0x90 + 1) P1_1;   /* TIN1 / AIN0  <- WS2812 データ線        */
__sbit __at(0x90 + 2) P1_2;
__sbit __at(0x90 + 3) P1_3;
__sbit __at(0x90 + 4) P1_4;   /* TIN2 / AIN1 / SCS                      */
__sbit __at(0x90 + 5) P1_5;   /* TIN3 / AIN2 / MOSI                     */
__sbit __at(0x90 + 6) P1_6;   /* TIN4 / MISO                            */
__sbit __at(0x90 + 7) P1_7;   /* TIN5 / SCK   <- タッチパッド           */
__sfr __at(0x92) P1_MOD_OC;   /* 0 = プッシュプル, 1 = オープンドレイン */
__sfr __at(0x93) P1_DIR_PU;

__sfr __at(0xA0) P2;

__sfr __at(0xB0) P3;
__sbit __at(0xB0 + 0) P3_0;
__sbit __at(0xB0 + 1) P3_1;
__sbit __at(0xB0 + 2) P3_2;
__sbit __at(0xB0 + 3) P3_3;
__sbit __at(0xB0 + 4) P3_4;
__sbit __at(0xB0 + 5) P3_5;
__sbit __at(0xB0 + 6) P3_6;   /* UDP (USB D+)                           */
__sbit __at(0xB0 + 7) P3_7;   /* UDM (USB D-)                           */
__sfr __at(0x96) P3_MOD_OC;
__sfr __at(0x97) P3_DIR_PU;

__sfr __at(0xC6) PIN_FUNC;

/* ------------------------------------------------------------------ */
/* タイマ 0 / 1                                                        */
/* ------------------------------------------------------------------ */
__sfr __at(0x88) TCON;
__sbit __at(0x88 + 7) TF1;
__sbit __at(0x88 + 6) TR1;
__sbit __at(0x88 + 5) TF0;
__sbit __at(0x88 + 4) TR0;
__sfr __at(0x89) TMOD;
__sfr __at(0x8A) TL0;
__sfr __at(0x8B) TL1;
__sfr __at(0x8C) TH0;
__sfr __at(0x8D) TH1;

/* ------------------------------------------------------------------ */
/* タイマ 2 (16bit オートリロード)                                     */
/* ------------------------------------------------------------------ */
__sfr __at(0xC8) T2CON;
__sbit __at(0xC8 + 7) TF2;     /* 溢れフラグ。ソフトでクリアする       */
__sbit __at(0xC8 + 2) TR2;     /* 起動                                 */
__sbit __at(0xC8 + 1) C_T2;    /* 0 = タイマ, 1 = カウンタ             */
__sbit __at(0xC8 + 0) CP_RL2;  /* 0 = オートリロード, 1 = キャプチャ   */
__sfr __at(0xC9) T2MOD;
__sfr __at(0xCA) RCAP2L;       /* リロード値。溢れ時にハードが再装填   */
__sfr __at(0xCB) RCAP2H;
__sfr __at(0xCC) TL2;
__sfr __at(0xCD) TH2;

/* ------------------------------------------------------------------ */
/* UART0                                                               */
/* ------------------------------------------------------------------ */
__sfr __at(0x98) SCON;
__sbit __at(0x98 + 1) TI;
__sbit __at(0x98 + 0) RI;
__sfr __at(0x99) SBUF;

/* ------------------------------------------------------------------ */
/* ADC / タッチキー                                                    */
/* ------------------------------------------------------------------ */
__sfr __at(0x80) ADC_CTRL;
__sbit __at(0x80 + 5) ADC_IF;
__sbit __at(0x80 + 4) ADC_START;
__sfr __at(0x9A) ADC_CFG;
__sfr __at(0x9F) ADC_DATA;

__sfr __at(0xC3) TKEY_CTRL;
__sfr __at(0xC4) TKEY_DATL;
__sfr __at(0xC5) TKEY_DATH;
#define bTKC_IF     0x80      /* 読出専用: 変換完了フラグ                */
#define bTKC_2MS    0x10      /* サンプル周期 1ms -> 2ms                 */
#define MASK_TKC_CH 0x07
#define bTKD_CHG    0x80      /* TKEY_DATH bit7: 制御変更直後で値が無効   */

/*
 * チャネル選択は 1 始まり。TIN 番号とレジスタ値が 1 ずれる。
 *   0 = 停止, 1 = TIN0(P1.0), 2 = TIN1(P1.1), 3 = TIN2(P1.4),
 *   4 = TIN3(P1.5), 5 = TIN4(P1.6), 6 = TIN5(P1.7), 7 = 有効だが未選択
 * したがって TIN5 を使うなら TKEY_CTRL の下位 3bit は 6。
 */
#define TKEY_CH_CODE(tin) ((uint8_t)((tin) + 1))

/* ------------------------------------------------------------------ */
/* USB デバイス                                                        */
/* ------------------------------------------------------------------ */
__sfr __at(0x91) USB_C_CTRL;
__sfr __at(0xD1) UDEV_CTRL;
__sfr __at(0xD2) UEP1_CTRL;
__sfr __at(0xD3) UEP1_T_LEN;
__sfr __at(0xD4) UEP2_CTRL;
__sfr __at(0xD5) UEP2_T_LEN;
__sfr __at(0xD6) UEP3_CTRL;
__sfr __at(0xD7) UEP3_T_LEN;
__sfr __at(0xD8) USB_INT_FG;
__sbit __at(0xD8 + 7) U_IS_NAK;
__sbit __at(0xD8 + 6) U_TOG_OK;
__sbit __at(0xD8 + 5) U_SIE_FREE;
__sbit __at(0xD8 + 4) UIF_FIFO_OV;
__sbit __at(0xD8 + 2) UIF_SUSPEND;
__sbit __at(0xD8 + 1) UIF_TRANSFER;
__sbit __at(0xD8 + 0) UIF_BUS_RST;
__sfr __at(0xD9) USB_INT_ST;
__sfr __at(0xDA) USB_MIS_ST;
__sfr __at(0xDB) USB_RX_LEN;
__sfr __at(0xDC) UEP0_CTRL;
__sfr __at(0xDD) UEP0_T_LEN;
__sfr __at(0xDE) UEP4_CTRL;
__sfr __at(0xDF) UEP4_T_LEN;
__sfr __at(0xE1) USB_INT_EN;
__sfr __at(0xE2) USB_CTRL;
__sfr __at(0xE3) USB_DEV_AD;
__sfr __at(0xE4) UEP2_DMA_L;
__sfr __at(0xE5) UEP2_DMA_H;
__sfr __at(0xE6) UEP3_DMA_L;
__sfr __at(0xE7) UEP3_DMA_H;
__sfr __at(0xEA) UEP4_1_MOD;
__sfr __at(0xEB) UEP2_3_MOD;
__sfr __at(0xEC) UEP0_DMA_L;
__sfr __at(0xED) UEP0_DMA_H;
__sfr __at(0xEE) UEP1_DMA_L;
__sfr __at(0xEF) UEP1_DMA_H;

/* UDEV_CTRL */
#define bUD_PD_DIS      0x80  /* UDP/UDM のプルダウンを切る              */
#define bUD_DP_PIN      0x20  /* 読出専用: UDP の現在レベル              */
#define bUD_DM_PIN      0x10  /* 読出専用: UDM の現在レベル              */
#define bUD_LOW_SPEED   0x04
#define bUD_GP_BIT      0x02
#define bUD_PORT_EN     0x01  /* 物理ポート有効                          */

/* USB_CTRL */
#define bUC_LOW_SPEED   0x40
#define bUC_DEV_PU_EN   0x20  /* デバイスモードで内蔵プルアップを有効化  */
#define bUC_SYS_CTRL1   0x20
#define bUC_SYS_CTRL0   0x10
#define MASK_UC_SYS_CTRL 0x30
#define bUC_INT_BUSY    0x08  /* 転送割込処理中は自動で NAK を返す       */
#define bUC_RESET_SIE   0x04
#define bUC_CLR_ALL     0x02
#define bUC_DMA_EN      0x01

/* USB_INT_EN */
#define bUIE_DEV_SOF    0x80
#define bUIE_DEV_NAK    0x40
#define bUIE_FIFO_OV    0x10
#define bUIE_SUSPEND    0x04
#define bUIE_TRANSFER   0x02
#define bUIE_BUS_RST    0x01

/* USB_DEV_AD */
#define MASK_USB_ADDR   0x7F

/* USB_MIS_ST (読出専用) */
#define bUMS_SIE_FREE    0x20
#define bUMS_R_FIFO_RDY  0x10
#define bUMS_BUS_RESET   0x08
#define bUMS_SUSPEND     0x04

/* USB_INT_ST (読出専用) */
#define bUIS_IS_NAK     0x80
#define bUIS_TOG_OK     0x40
#define MASK_UIS_TOKEN  0x30
#define UIS_TOKEN_OUT   0x00
#define UIS_TOKEN_SOF   0x10
#define UIS_TOKEN_IN    0x20
#define UIS_TOKEN_SETUP 0x30
#define MASK_UIS_ENDP   0x0F

/* UEPn_CTRL。応答種別は「次にホストへ返す handshake」を意味する */
#define bUEP_R_TOG      0x80  /* OUT で期待するデータトグル              */
#define bUEP_T_TOG      0x40  /* IN で送るデータトグル                   */
#define bUEP_AUTO_TOG   0x10  /* EP1-3 のみ: 転送成功で自動トグル        */
#define MASK_UEP_R_RES  0x0C
#define UEP_R_RES_ACK   0x00
#define UEP_R_RES_TOUT  0x04
#define UEP_R_RES_NAK   0x08
#define UEP_R_RES_STALL 0x0C
#define MASK_UEP_T_RES  0x03
#define UEP_T_RES_ACK   0x00
#define UEP_T_RES_TOUT  0x01
#define UEP_T_RES_NAK   0x02
#define UEP_T_RES_STALL 0x03

/* UEP4_1_MOD / UEP2_3_MOD */
#define bUEP1_RX_EN     0x80
#define bUEP1_TX_EN     0x40
#define bUEP1_BUF_MOD   0x10
#define bUEP4_RX_EN     0x08
#define bUEP4_TX_EN     0x04
#define bUEP3_RX_EN     0x80
#define bUEP3_TX_EN     0x40
#define bUEP3_BUF_MOD   0x10
#define bUEP2_RX_EN     0x08
#define bUEP2_TX_EN     0x04
#define bUEP2_BUF_MOD   0x01

/*
 * バッファ配置 (データシート 表 16.3.3「端点 n 缓冲区模式」)。
 *
 * 「以 UEPn_DMA 为起始地址由低向高排列」= UEPn_DMA から低位→高位へ、
 * **有効にした方向のぶんだけ**が順に並ぶ。並び順は受信 (OUT) が先。
 *
 *   RX_EN TX_EN BUF_MOD   配置                             必要な大きさ
 *     1     0     0       +0 受信                          64
 *     0     1     0       +0 **送信**                      64
 *     1     1     0       +0 受信 / +64 送信               128
 *
 * つまり **送信位置は「その端点で受信も有効かどうか」で決まる**。
 * 送信しかしない端点の送信データは +0 に置く。ここを一律 +64 だと
 * 思い込むと、確保していない領域を SIE が読んで送るので、
 * 列挙は通るのにレポートだけ届かないという嫌な症状になる。
 *
 * 逆に受信も有効な端点で +0 に書くと、送るつもりのデータを
 * 受信領域に置くことになって同じ症状になる。どちらの向きにも
 * 間違えられるので、offset は MOD レジスタに書く値から機械的に
 * 導くこと (src/usb.c を参照)。
 *
 * WCH 自身の例でも、IN のみの EP1 は memcpy(Ep1Buffer, ...) で +0、
 * 双方向の EP2 は Ep2Buffer+MAX_PACKET_SIZE で +64 と使い分けている。
 */
#define UEP_DIR_SIZE    64   /* 有効な 1 方向あたりのバッファ長 */

#endif /* CH552_H */
