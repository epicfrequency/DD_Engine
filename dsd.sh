#!/bin/bash
USER_GAIN=${1:-0.5}
PIPE_PATH="/tmp/mpd_dsd_pipe"
ENGINE_BIN="./lumen_dsd_engine"
SOURCE_CODE="lumen_dsd_engine.cpp"

# 针对 Pi 5 64位系统的究极编译
g++ -O3 -march=native -mcpu=cortex-a76 -flto \
    -std=c++17 -fomit-frame-pointer -fno-stack-protector \
    "$SOURCE_CODE" -o "$ENGINE_BIN"

killall -9 aplay "$(basename $ENGINE_BIN)" 2>/dev/null
[ -p "$PIPE_PATH" ] || mkfifo "$PIPE_PATH"

# 768kHz 封装 DSD512 U32_BE 输出
cat "$PIPE_PATH" | \
taskset -c 2,3 chrt -f 99 "$ENGINE_BIN" "$USER_GAIN" | \
aplay -D hw:0,0 -c 2 -f DSD_U32_BE -r 768000 -B 200000 --period-size=50000 -q 2>/dev/null &