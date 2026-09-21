/* SPDX-License-Identifier: MIT */
#include "ch552.h"
#include "dflash.h"

/*
 * Data-Flash は 0xC000 から 128 バイトぶんだが、
 * 「偶数アドレスのみ有効」なので添字を 1bit 左シフトして渡す。
 * ここを 1 対 1 で写すと 64 バイトしか見えず、しかも
 * 一つ飛ばしに化けるという分かりにくい壊れ方をする。
 */
#define DF_ADDR_L(idx) ((uint8_t)((idx) << 1))
#define DF_ADDR_H      ((uint8_t)(DATA_FLASH_ADDR >> 8))

uint8_t dflash_read(uint8_t idx)
{
    if (idx >= DATA_FLASH_SIZE) {
        return 0xFF;
    }
    ROM_ADDR_H = DF_ADDR_H;
    ROM_ADDR_L = DF_ADDR_L(idx);
    ROM_CTRL = ROM_CMD_READ;
    return ROM_DATA_L;
}

uint8_t dflash_write(uint8_t idx, uint8_t val)
{
    uint8_t status;
    uint8_t saved_ea;

    if (idx >= DATA_FLASH_SIZE) {
        return 0;
    }

    /*
     * セーフモードの解錠 (0x55, 0xAA) は「連続して書く」ことが条件で、
     * 途中に別の処理が挟まると開かない。ここは割り込みが有効なまま
     * 呼ばれるので、1ms のタイマ割り込みや USB 転送完了割り込みが
     * 2 つの書き込みの間に入るとその都度失敗する。
     * 症状は「たまに保存できない」で、再現条件が見えにくい。
     *
     * 割り込みを止めるのは解錠の前後だけにする。フラッシュの書き込み
     * サイクル自体は ms のオーダーになりうるので、そこまで含めて
     * 止めると Timer2 の溢れフラグは 1 本しかない以上 tick を落とす。
     * 閾値保存は 6 バイトぶんあるので、そのたびに時計が数 ms 遅れる。
     * ROM_ADDR / ROM_DATA / ROM_CTRL は他の誰も触らないので、
     * 割り込みが挟まっても壊れない。
     */
    saved_ea = EA;

    /* 書き換え許可。bCODE_WE は絶対に立てない (コード領域が飛ぶ) */
    EA = 0;
    SAFE_MOD = 0x55;
    SAFE_MOD = 0xAA;
    GLOBAL_CFG |= bDATA_WE;
    SAFE_MOD = 0x00;
    EA = saved_ea;

    ROM_ADDR_H = DF_ADDR_H;
    ROM_ADDR_L = DF_ADDR_L(idx);
    ROM_DATA_L = val;
    ROM_CTRL = ROM_CMD_WRITE;
    status = ROM_STATUS;

    EA = 0;
    SAFE_MOD = 0x55;
    SAFE_MOD = 0xAA;
    GLOBAL_CFG &= ~bDATA_WE;
    SAFE_MOD = 0x00;
    EA = saved_ea;

    if ((status & (bROM_ADDR_OK | bROM_CMD_ERR)) != bROM_ADDR_OK) {
        return 0;
    }

    /*
     * ステータスが見ているのは「アドレスが有効だったか」と
     * 「コマンドを知っているか」だけで、セルに本当に入ったかは
     * 分からない。Data-Flash は約 1 万回で寿命が来るので、
     * 摩耗したセルに書いたときは静かに古い値のまま残る。
     * 読み返して確かめる。
     */
    return (uint8_t)(dflash_read(idx) == val);
}
