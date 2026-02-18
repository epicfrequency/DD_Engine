#!/bin/bash

# 1. 获取 MPD 当前输出的目标采样率 (通过 mpc 快速查询)
# 因为 MPD 在启动 pipe 之前已经确定了频率，所以这里读到的是准确的
RATE=$(mpc -f %samplerate% current | cut -d':' -f1)

# 2. 根据 MPD 输出的 PCM 频率，设定 DSD512 的硬件时钟
if [ "$RATE" = "352800" ]; then
    CLOCK=705600
    ccho "DSD512 22.824Mhz"
else
    # 默认为 48k 系，即 384000 -> 24576000
    CLOCK=768000
    echo "DSD512 24.576Mhz"
fi


# 3. 启动链路：从 stdin 读数据 -> 调制引擎 -> aplay
# 优化 buffer 防止声音乱掉，使用 --buffer-time 更稳
exec /usr/local/bin/sdm5_mt 0.2 | aplay -D hw:0,0 -c 2 -f DSD_U32_BE -r $CLOCK --buffer-size=8192 -M

