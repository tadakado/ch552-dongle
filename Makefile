# SPDX-License-Identifier: MIT
#
# CH552 ファームウェア ビルド
#
#   make          ビルド (build/fw.bin まで)
#   make flash    ビルドして書き込み
#   make size     コードサイズと残量を表示
#   make clean
#
# 必要なもの (macOS):
#   brew install sdcc
#   uv tool install ch55xtool     # 書き込みツール。GPL だが独立プログラム
#
# SDCC 4.2.0 と 4.5.0 でビルドを確認済み (全設定の組み合わせ、警告ゼロ)。
# 4.5.0 の方が最適化が進んでいて 40 バイトほど小さくなる。
# インラインアセンブラの .rept は両方で正しく展開される。
# 別のバージョンを使う場合は make timing でビットタイミングを、
# 記述子は ihx から読み直して検算し直すこと。

TARGET   := fw
BUILD    := build

# --- ターゲット定義 --------------------------------------------------
# CH552 のフラッシュは 16KB だが、上位 2KB (0x3800-0x3FFF) は
# ブートローダが占有しているので、ユーザ領域は 14KB = 0x3800。
# 動作周波数は config.h の FREQ_SYS だけを出所にする。ここに同じ値を
# 書くと、片方だけ変えたときに「実機と検査で周波数が違うのに合格が出る」
# ことになる。検査ツール (tools/*.py) も config.h を読む。
CODE_SIZE  := 0x3800
# xRAM は 1KB (0x0000-0x03FF)。前半 0x0000-0x013F は USB の端点バッファに使うため
# (src/usb.c が __at で固定配置している)、SDCC 自身の xdata 割り当ては
# 0x0140 以降に追い出す。config.h の USB_RAM_END と揃えること。
# 端点バッファは wMaxPacketSize に合わせて詰めてあり、以前の決め打ち
# (全部 128 バイト) に比べて 192 バイトを SDCC 側に返している。
XRAM_LOC   := 0x0140
XRAM_SIZE  := 0x02C0
IRAM_SIZE  := 0x0100

# --- ツール ----------------------------------------------------------
SDCC     := sdcc
PACKIHX  := packihx
OBJCOPY  := sdobjcopy
FLASHER  := ch55xtool

# sort を通すのは並びを固定するため。GNU make の $(wildcard) は
# ディレクトリ順を返すことがあり (macOS の古い make がそう)、
# 環境によってリンク順が変わる。このプロジェクトはコードの番地が
# WS2812 のビット周期に効く (DJNZ の分岐先が奇数番地だと 1 サイクル
# 増える) ので、並びは決め打ちにしておく。
SRCS := $(sort $(wildcard src/*.c))
RELS := $(patsubst src/%.c,$(BUILD)/%.rel,$(SRCS))

CFLAGS := -mmcs51 --model-small \
          --code-size $(CODE_SIZE) \
          --xram-size $(XRAM_SIZE) --xram-loc $(XRAM_LOC) \
          --iram-size $(IRAM_SIZE) \
          --std-c99 \
          -MMD \
          -I include -I src -I .

LDFLAGS := $(CFLAGS)

.PHONY: all flash size timing hex clean

all: $(BUILD)/$(TARGET).bin

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.rel: src/%.c | $(BUILD)
	$(SDCC) $(CFLAGS) -c $< -o $@

# ヘッダの依存関係。これが無いと config.h を書き換えても再ビルドされず、
# 直したつもりの古いバイナリを焼くことになる。ボード立ち上げで一番
# 触るのが config.h なので、ここは黙って踏む。
# SDCC の -MMD が build/*.d を吐くので取り込む。
-include $(RELS:.rel=.d)

# main.rel を先頭に置くこと。SDCC は最初に渡した .rel のモジュールに
# 割り込みベクタ表を作るので、main を含むものが先頭でないとベクタが
# 張られない。順序を決めているのはこの行。
$(BUILD)/$(TARGET).ihx: $(BUILD)/main.rel $(filter-out $(BUILD)/main.rel,$(RELS))
	$(SDCC) $(LDFLAGS) -o $@ $^

# .hex は ISP ツールによっては欲しがるので作れるようにしてある。
# 既定のビルドでは使わない (make flash は .bin を渡す)。
hex: $(BUILD)/$(TARGET).hex

$(BUILD)/$(TARGET).hex: $(BUILD)/$(TARGET).ihx
	$(PACKIHX) $< > $@

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).ihx
	@if command -v $(OBJCOPY) >/dev/null 2>&1; then \
		$(OBJCOPY) -I ihex -O binary $< $@; \
	else \
		makebin -p < $< > $@; \
	fi
	@$(MAKE) --no-print-directory size

size: $(BUILD)/$(TARGET).ihx
	@if [ -f $(BUILD)/$(TARGET).mem ]; then \
		echo "--- memory ---"; \
		grep -E 'ROM/EPROM/FLASH|EXTERNAL RAM|Stack starts' $(BUILD)/$(TARGET).mem; \
		echo "(ユーザ領域上限 $(CODE_SIZE) = 14336 bytes)"; \
	fi

# 生成物からサイクル数を数え直す検査。NOP 数や delay の形を変えたら
# 必ずこれを通すこと。周波数は config.h から読む。
timing: $(BUILD)/$(TARGET).bin
	python3 tools/neo_timing.py
	@echo
	python3 tools/delay_check.py
	@echo
	python3 tools/naked_check.py
	@echo
	python3 tools/oseg_check.py

# タッチの前段フィルタを机上で回す。前段を触ったら通すこと。
# 生成物に依存しないので timing とは分けてある。
.PHONY: touchsim
touchsim:
	python3 tools/touch_sim.py

# 検査を通っていない .bin は焼かせない。周期はリンク配置で変わりうる
# ものなので、焼く直前に必ず数え直す。
flash: timing
	$(FLASHER) -f $(BUILD)/$(TARGET).bin

clean:
	rm -rf $(BUILD)
