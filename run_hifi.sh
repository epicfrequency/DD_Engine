#!/bin/bash

USER_GAIN=${1:-0.50}
PIPE_PATH="/tmp/mpd_dsd_pipe"
ENGINE_BIN="./lumen_dsd_engine"
SOURCE_CODE="lumen_dsd_engine.cpp"

# 1. 自动编译
g++ -O3 -march=native -ffast-math -funroll-loops "$SOURCE_CODE" -o "$ENGINE_BIN"

# 2. 硬件资源清理
killall -9 aplay "$ENGINE_BIN" 2>/dev/null
[ -p "$PIPE_PATH" ] || mkfifo "$PIPE_PATH"

# 3. 启动 768kHz DSD512 链路
# 注意：使用 -f DSD_U32_BE 或 S32_LE 取决于你的 DAC 驱动
cat "$PIPE_PATH" | \
taskset -c 2,3 chrt -f 99 "$ENGINE_BIN" "$USER_GAIN" | \
aplay -D hw:1,0 -c 2 -f DSD_U32_BE -r 768000 -q -B 100000 2>/dev/null