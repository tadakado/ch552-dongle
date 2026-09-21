/* SPDX-License-Identifier: MIT */
/*
 * cdc.h - CDC-ACM のシリアルコンソール
 *
 * 送受信ともリングバッファを挟んでいる。ホストがポートを開いて
 * いないときは送信バッファが埋まって古い順に捨てられるだけで、
 * ファーム側は止まらない。
 */
#ifndef CDC_H
#define CDC_H

#include <stdint.h>

/* usb.c の割り込みから呼ばれる。ホストから届いたバイトを受け取る */
void cdc_on_rx(__xdata uint8_t *data, uint8_t len);

/* 端点が初期化されたときに呼ぶ (バスリセット / SET_CONFIGURATION) */
void cdc_reset(void);

/* 受信リングの空きに応じて EP2 OUT の応答 (ACK/NAK) を決め直す */
void cdc_rx_rearm(void);

/* ポートが閉じられたときに、送信待ちを捨てる */
void cdc_tx_drop(void);

/* メインループから毎周回呼ぶ。溜まった送信データを EP2 に流す */
void cdc_flush(void);

/* 受信が 1 バイトあれば取り出して 0..255 を返す。無ければ -1 */
int16_t cdc_getc(void);

void cdc_putc(char c);
void cdc_puts(__code const char *s);   /* 文字列リテラルを直接渡せる */
void cdc_put_u16(uint16_t v);
void cdc_put_nl(void);

#endif /* CDC_H */
