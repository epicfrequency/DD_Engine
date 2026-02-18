#include <iostream>
#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>

// 保持 64 字节对齐，防止多线程下的缓存行失效（虽然是单线程，但这是好习惯）
struct alignas(64) SDM5 {
    double s[5] = {0,0,0,0,0};
    double q = 0;
    const double LIMIT = 100.0;
    double gain_factor = 0.5;

    // 极致优化：去掉所有监控计数、Peak 记录和 Reset 逻辑
    inline int modulate(double input) {
        const double x = input * gain_factor;
        
        // --- 核心音频链路逻辑：绝对不动 ---
        s[0] += (x - q);
        s[1] += (s[0] - q * 0.5);
        s[2] += (s[1] - q * 0.25);
        s[3] += (s[2] - q * 0.125);
        s[4] += (s[3] - q * 0.0625);

        // 仅保留必要的 Hard Clip 保护，防止数值飞出 double 范围
        for (int i = 0; i < 5; ++i) {
            if (s[i] > LIMIT) s[i] = LIMIT;
            else if (s[i] < -LIMIT) s[i] = -LIMIT;
        }

        int bit = (s[4] >= 0) ? 1 : 0;
        q = bit ? 1.0 : -1.0;
        return bit;
    }
};

int main(int argc, char* argv[]) {
    double target_gain = (argc > 1) ? std::atof(argv[1]) : 0.5;

    // 提升 IO 效率
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(NULL);

    SDM5 mod_l, mod_r;
    mod_l.gain_factor = mod_r.gain_factor = target_gain;

    float cur[2], nxt[2];
    uint8_t out_l[8], out_r[8];

    // 预读取第一帧
    if (!std::cin.read(reinterpret_cast<char*>(cur), 8)) return 0;

    // 彻底移除 stderr 的实时 UI 渲染，只跑音频逻辑
    while (std::cin.read(reinterpret_cast<char*>(nxt), 8)) {
        
        // DSD512 64倍插值处理
        for (int i = 0; i < 8; ++i) {
            uint8_t bl = 0, br = 0;
            for (int bit = 7; bit >= 0; --bit) {
                // 线性插值
                float alpha = static_cast<float>(i * 8 + (7 - bit)) / 64.0f;
                double pl = (double)cur[0] * (1.0f - alpha) + (double)nxt[0] * alpha;
                double pr = (double)cur[1] * (1.0f - alpha) + (double)nxt[1] * alpha;
                
                if (mod_l.modulate(pl)) bl |= (1 << bit);
                if (mod_r.modulate(pr)) br |= (1 << bit);
            }
            out_l[i] = bl; out_r[i] = br;
        }

        // 紧凑的输出逻辑：对齐 DSD512 的双声道交织格式
        std::cout.write(reinterpret_cast<char*>(&out_l[0]), 4);
        std::cout.write(reinterpret_cast<char*>(&out_r[0]), 4);
        std::cout.write(reinterpret_cast<char*>(&out_l[4]), 4);
        std::cout.write(reinterpret_cast<char*>(&out_r[4]), 4);

        cur[0] = nxt[0]; cur[1] = nxt[1];
    }
    return 0;
}