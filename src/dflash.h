/* SPDX-License-Identifier: MIT */
/*
 * dflash.h - CH552 内蔵 Data-Flash (128 バイト) の読み書き
 *
 * 用途はタッチキーの閾値のような「基板ごとに変わるが滅多に書き換えない」値。
 * 書き換え可能回数は有限なので、毎フレーム書くような使い方はしないこと。
 */
#ifndef DFLASH_H
#define DFLASH_H

#include <stdint.h>

/* idx は 0..127 のバイト添字 (アドレス変換は実装側でやる) */
uint8_t dflash_read(uint8_t idx);

/* 成功したら 1。アドレス不正・コマンドエラーなら 0 */
uint8_t dflash_write(uint8_t idx, uint8_t val);

#endif /* DFLASH_H */
