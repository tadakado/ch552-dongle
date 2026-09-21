/* SPDX-License-Identifier: MIT */
/*
 * touch.h - CH552 内蔵タッチキー (TIN5 / P1.7) ドライバ
 *
 * 閾値は 3 段構えで決まる。優先順位は上から:
 *   1. Data-Flash に保存された値 (実機ごとに後から書き換えられる)
 *   2. config.h の TOUCH_THRESHOLD_DEFAULT (0 以外なら固定値として使う)
 *   3. 起動時のノイズ実測からの自動決定
 * どれになったかは touch_threshold_source() で分かる。
 */
#ifndef TOUCH_H
#define TOUCH_H

#include <stdint.h>

typedef enum {
    TOUCH_THR_AUTO = 0,   /* 起動時のノイズ測定から決めた */
    TOUCH_THR_CONFIG,     /* config.h の固定値            */
    TOUCH_THR_FLASH,      /* Data-Flash に保存された値    */
    /*
     * いま動いている値が Data-Flash の記録と一致していない。
     * コンソールの + / - で動かした直後か、保存に失敗して記録の方が
     * 壊れた場合。これを分けないと、`i` が「src=flash」と言いながら
     * 電源を入れ直すと違う値で起動する、という嘘になる。
     */
    TOUCH_THR_UNSAVED
} touch_thr_src_t;

/*
 * 保存の結果。0 以外は「その値がいま有効で、Data-Flash にも入っている」
 * を意味する。SAME を分けているのは、学習ジェスチャの回数制限が
 * 「実際に焼いたときだけ減る」ようにするため。
 */
typedef enum {
    TOUCH_SAVE_FAIL = 0,  /* 焼けなかった。動作中の閾値も変わらない  */
    TOUCH_SAVE_WROTE,     /* 焼いた                                  */
    TOUCH_SAVE_SAME       /* 既に同じ値が入っていた。焼いていない    */
} touch_save_t;

/*
 * チャネル設定、ベースライン取得、閾値決定までやる。
 * 指を触れていない状態で呼ぶこと (ベースラインを測るため)。
 */
void touch_init(void);

/* メインループから毎周回呼ぶ。1ms に 1 サンプルだけ取り込む */
void touch_poll(void);

/* --- 状態の取得 -------------------------------------------------- */
uint8_t  touch_is_down(void);        /* 現在触れているか              */
uint8_t  touch_take_press(void);     /* 押し下げエッジを 1 回だけ拾う */

/*
 * 溜まっている押下 / 離しイベントを捨てる。
 * サスペンドに入る境目で、過去の操作が持ち越されるのを防ぐ。
 */
void     touch_clear_events(void);
uint8_t  touch_take_release(void);   /* 離しエッジを 1 回だけ拾う     */
/*
 * 押しっぱなしの継続時間。65.5 秒で一周するが、その前に
 * 押しっぱなし救済 (TOUCH_STUCK_MS = 30 秒) が離しを立てる。
 */
uint16_t touch_hold_ms(void);

/* --- 較正用の生データ -------------------------------------------- */
uint16_t touch_raw(void);            /* 平滑後の生カウント            */
uint16_t touch_baseline(void);       /* 追従中のベースライン          */
uint16_t touch_delta(void);          /* baseline - raw (触ると増える) */
uint16_t touch_noise(void);          /* 起動時に測ったノイズ幅        */
uint16_t touch_peak_delta(void);     /* 直近の押下中に出た delta 最大 */

/* --- 閾値の操作 -------------------------------------------------- */
uint16_t        touch_threshold(void);
touch_thr_src_t touch_threshold_source(void);

/*
 * 実行時に閾値を差し替える。保存はしない。
 * TOUCH_THRESHOLD_MIN..MAX の外は**丸める** (拒否しない)。
 * 候補として妥当かを見たいときは touch_save_threshold() の方を使うこと。
 * あちらは範囲外を拒否する。
 */
void touch_set_threshold(uint16_t thr);

/*
 * thr を Data-Flash に保存する。
 *
 * 戻り値が 0 以外なら「thr がいま有効で、Data-Flash にも入っている」。
 * 焼いた場合と、既に同じ値が入っていて焼く必要が無かった場合を
 * 分けてあるが、成否として見るぶんには 0 かどうかだけでよい。
 * 失敗したときは動作中の閾値も変わらないので、呼び出し側は戻り値を
 * そのまま「採用できたか」として扱ってよい。
 *
 * 制限は掛けていない。学習ジェスチャからは touch_learn_threshold() を
 * 使うこと。
 */
touch_save_t touch_save_threshold(uint16_t thr);

/*
 * 学習ジェスチャ用の保存。touch_save_threshold() に
 * 「電源 ON から TOUCH_LEARN_MAX_WRITES 回まで」の制限を足したもの。
 * 上限に達していると、焼かずに TOUCH_SAVE_FAIL を返す。
 *
 * 同じ値のときは焼かないので回数も減らない。判定の順序が肝で、
 * 上限を先に見ると「既に同じ値が入っているのに上限切れで不採用」に
 * なり、表示 (黄) と実際の閾値が食い違う。
 */
touch_save_t touch_learn_threshold(uint16_t thr);

/* 学習ジェスチャで焼いた回数 (電源 ON からの通算) */
uint8_t touch_learn_writes(void);

/*
 * Data-Flash の保存値を無効化して、次回起動時に自動決定へ戻す。
 * 動作中の閾値はそのまま。src は flash から unsaved に落ちる
 * (記録が無くなった以上、動いている値はもう Data-Flash 由来ではない)。
 */
uint8_t touch_clear_threshold(void);

/*
 * 直近の押下で観測した delta のピークから閾値の候補を作る。
 * **副作用は無い**。採用するなら touch_save_threshold() か
 * touch_set_threshold() に渡すこと。ピークが小さすぎて信用できない
 * 場合は 0 を返す。
 */
uint16_t touch_peak_threshold(uint8_t divisor);

/* ベースラインを取り直す (触れていない状態で呼ぶこと) */
void touch_recalibrate(void);

#endif /* TOUCH_H */
