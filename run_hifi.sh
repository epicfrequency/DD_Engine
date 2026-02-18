#!/bin/bash

# MPD config

# audio_output {
#     type        "pipe"
#     name        "DSD512_Upsampler"
#     command     "cat > /tmp/mpd_dsd_pipe"
#       format      "384000:f:2"
#   # 这里固定 352.8k Float
#     #mixer_type      "software"
# }

# resampler {
#         plugin  "soxr"
#         quality "very high"
# }


# 参数设置
USER_GAIN=${1:-0.2} # 默认增益设置为 0.2，用户可通过命令行参数覆盖
PIPE_PATH="/tmp/mpd_dsd_pipe"
ENGINE_BIN="./lumen_dsd_engine"
SOURCE_CODE="lumen_dsd_engine.cpp"

echo "----------------------------------------------------"
echo "   LUMIN DSD512 Hi-Fi Engine for Pi 5 (ARM64)"
echo "----------------------------------------------------"

# 1. 编译阶段
echo "[1/4] 正在进行究极优化编译 (Cortex-A76 + LTO)..."
g++ -O3 -march=native -mcpu=cortex-a76 -flto \
    -std=c++17 -fomit-frame-pointer -fno-stack-protector \
    "$SOURCE_CODE" -o "$ENGINE_BIN"

if [ $? -eq 0 ]; then
    echo "  >> ✅ 编译成功：指令集已针对 Pi 5 硬件加速对齐。"
else
    echo "  >> ❌ 编译失败，请检查代码。"
    exit 1
fi

# 2. 清理与准备
echo "[2/4] 正在清理旧进程并挂载管道..."
killall -9 aplay "$(basename $ENGINE_BIN)" 2>/dev/null
[ -p "$PIPE_PATH" ] || mkfifo "$PIPE_PATH"
echo "  >> 管道就绪: $PIPE_PATH"

# 3. 硬件检查
echo "[3/4] 正在探测 DAC 声卡 (hw:0,0)..."
if aplay -l | grep -q "card 0"; then
    CARD_INFO=$(aplay -l | grep "card 0" | head -n 1)
    echo "  >> ✅ 发现声卡: $CARD_INFO"
else
    echo "  >> ❌ 错误：未发现 hw:0,0 设备，请检查 DAC 连接。"
    exit 1
fi

# 4. 启动链路
echo "[4/4] 正在点火 DSD512 链路 (768kHz PCM 封装)..."
echo "  >> 增益设置: $USER_GAIN"
echo "  >> 调度策略: 实时优先级 99 + 物理大核 (Core 2,3) 锁定"

# 启动核心逻辑
cat "$PIPE_PATH" | \
taskset -c 2,3 chrt -f 99 "$ENGINE_BIN" "$USER_GAIN" | \
aplay -D hw:0,0 \
      -c 2 \
      -f DSD_U32_BE \
      -r 768000 \
      -B 200000 \
      --period-size=50000 \
      -q 2>/dev/null &

if [ $? -eq 0 ]; then
    echo "----------------------------------------------------"
    echo "   🚀 引擎已进入后台运行模式！享用你的 Hi-Fi 音乐吧。"
    echo "   提示: CPU 占用率预期在 40% 左右。"
    echo "----------------------------------------------------"
else
    echo "❌ 启动失败，请检查 ALSA 参数。"
fi