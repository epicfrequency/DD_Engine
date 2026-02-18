#!/bin/bash

# 参数设置
USER_GAIN=${1:-0.2}
PIPE_PATH="/tmp/mpd_dsd_pipe"
ENGINE_BIN="./lumen_dsd_engine"
SOURCE_CODE="lumen_dsd_engine.cpp"

echo "===================================================="
echo "      LUMIN DSD512 HIGH-PERFORMANCE ENGINE"
echo "      Architecture: ARM64 (Cortex-A76 Optimized)"
echo "===================================================="

# 1. 究极编译
echo "[Step 1] 正在执行 FP32 硬件加速编译..."
# 加入 -s 参数减小二进制体积，进一步优化指令缓存
g++ -Ofast -march=native -mcpu=cortex-a76 -flto -s \
    -std=c++17 -fomit-frame-pointer -fno-stack-protector \
    "$SOURCE_CODE" -o "$ENGINE_BIN"

if [ $? -eq 0 ]; then
    echo "  >> ✅ 编译完成。已锁定 Pi 5 最佳指令集。"
else
    echo "  >> ❌ 编译失败，检查代码语法。"
    exit 1
fi

# 2. 资源清理
killall -9 aplay "$(basename $ENGINE_BIN)" 2>/dev/null
[ -p "$PIPE_PATH" ] || mkfifo "$PIPE_PATH"

# 3. 核心分配提示
echo "[Step 2] 正在配置系统实时资源..."
echo "  >> 绑定大核: Core 2 & 3"
echo "  >> 实时优先级: FIFO 99"
echo "  >> 输出格式: DSD_U32_BE (768kHz Encap)"

# 4. 启动启动
echo "[Step 3] 链路点火..."
cat "$PIPE_PATH" | \
taskset -c 2,3 chrt -f 99 "$ENGINE_BIN" "$USER_GAIN" | \
aplay -D hw:0,0 \
      -c 2 \
      -f DSD_U32_BE \
      -r 768000 \
      -B 250000 \
      --period-size=62500 \
      -q 2>/dev/null &

# 5. 状态确认
sleep 1
if pgrep -x "$(basename $ENGINE_BIN)" > /dev/null; then
    CPU_USAGE=$(top -bn1 | grep "$(basename $ENGINE_BIN)" | awk '{print $9}')
    echo "===================================================="
    echo "  >> 🚀 引擎运行中！"
    echo "  >> 当前预估单核负载: ~${CPU_USAGE:-50}%"
    echo "  >> 提示: 若出现断音，请尝试略微增大 -B 参数。"
    echo "===================================================="
else
    echo "❌ 启动异常，请检查 ALSA 设备占用情况。"
fi