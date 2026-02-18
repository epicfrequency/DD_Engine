#!/bin/bash

# 参数设置
USER_GAIN=${1:-0.2}
PIPE_PATH="/tmp/mpd_dsd_pipe"
ENGINE_BIN="./lumen_dsd_engine"
SOURCE_CODE="lumen_dsd_engine.cpp"

echo "[Step 1] 清理旧进程与硬件资源..."
killall -9 aplay "$ENGINE_BIN" 2>/dev/null
[ -p "$PIPE_PATH" ] || mkfifo "$PIPE_PATH"

echo "[Step 2] 正在编译音频引擎 (Pi 5 极致优化模式)..."
g++ -O3 -march=native -ffast-math -funroll-loops "$SOURCE_CODE" -o "$ENGINE_BIN"
if [ $? -ne 0 ]; then
    echo "❌ 编译失败，请检查 C++ 代码。"
    exit 1
fi

echo "[Step 3] 验证声卡 hw:0,0 状态..."
# 检查设备是否被占用或在线
if ! aplay -l | grep -q "card 0"; then
    echo "❌ 未找到声卡 hw:0,0，请确认设备连接。"
    exit 1
fi

echo "[Step 4] 启动 DSD512 链路 (768kHz)..."
# 绑定核心 2,3 并设置实时优先级 99
cat "$PIPE_PATH" | \

#768000 means DSD512
taskset -c 2,3 chrt -f 99 "$ENGINE_BIN" "$USER_GAIN" | \
aplay -D hw:0,0 -c 2 -f DSD_U32_BE -r 768000 -q -B 100000 --period-size=25000 2>/dev/null