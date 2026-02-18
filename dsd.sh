#!/bin/bash

# 参数
USER_GAIN=${1:-0.5}
PIPE_PATH="/tmp/mpd_dsd_pipe"
ENGINE_BIN="./lumen_dsd_engine"
SOURCE_CODE="lumen_dsd_engine.cpp"

# [究极编译参数]
# -Ofast: 开启所有优化及非标准数学优化
# -mcpu=cortex-a76: 针对 Pi 5 核心进行微架构优化
# -flto: 全局链接时优化，消除函数调用开销
# -fno-stack-protector: 去掉栈保护，榨取最后一点频率
g++ -Ofast -march=native -mcpu=cortex-a76 -flto -fno-stack-protector \
    -mfloat-abi=hard -mfpu=neon-fp-armv8 \
    "$SOURCE_CODE" -o "$ENGINE_BIN"

# 清理并创建管道
killall -9 aplay "$(basename $ENGINE_BIN)" 2>/dev/null
[ -p "$PIPE_PATH" ] || mkfifo "$PIPE_PATH"

# 启动链路
# 使用 chrt -f 99 赋予最高实时权限
# 使用 taskset -c 3 将计算密集型任务完全锁定在最后一个大核上（减少中断干扰）
cat "$PIPE_PATH" | \
taskset -c 3 chrt -f 99 "$ENGINE_BIN" "$USER_GAIN" | \
aplay -D hw:0,0 \
      -c 2 \
      -f DSD_U32_BE \
      -r 768000 \
      -B 200000 \
      --period-size=50000 \
      -q 2>/dev/null &