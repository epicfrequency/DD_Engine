#include <iostream>
#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <iomanip>

// 保持 64 字节对齐，优化缓存行利用率 ⚡
struct alignas(64) SDM5 {
    double s[5] = {0,0,0,0,0};
    double q = 0;
    const double LIMIT = 100.0;
    double gain_factor = 0.5;

    // 极致监控变量：只存储原始数据
    uint64_t total_samples = 0;
    uint64_t s4_clip_hits = 0; 
    double current_s4 = 0;
    double pcm_sample = 0;

    inline int modulate(double input) {
        const double x = input * gain_factor;
        total_samples++;

        // 1. 核心五阶调制算法 (纯算术)
        s[0] += (x - q);
        s[1] += (s[0] - q * 0.5);
        s[2] += (s[1] - q * 0.25);
        s[3] += (s[2] - q * 0.125);
        s[4] += (s[3] - q * 0.0625);

        // 2. 极致简化的 Clamp 逻辑
        // 前四阶只 Clamp 不计数
        for (int i = 0; i < 4; ++i) {
            if (s[i] > LIMIT) s[i] = LIMIT;
            else if (s[i] < -LIMIT) s[i] = -LIMIT;
        }

        // 仅对最后一阶 S4 进行截断统计 🎯
        if (s[4] > LIMIT) { s[4] = LIMIT; s4_clip_hits++; }
        else if (s[4] < -LIMIT) { s[4] = -LIMIT; s4_clip_hits++; }

        // 3. 记录瞬时值用于异步显示
        current_s4 = s[4];
        pcm_sample = x;

        int bit = (s[4] >= 0) ? 1 : 0;
        q = bit ? 1.0 : -1.0;
        return bit;
    }

    void reset_metrics() {
        s4_clip_hits = 0;
        total_samples = 0;
    }
};

int main(int argc, char* argv[]) {
    double target_gain = (argc > 1) ? std::atof(argv[1]) : 0.5;

    // 提升 IO 吞吐量
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(NULL);

    SDM5 mod_l, mod_r;
    mod_l.gain_factor = mod_r.gain_factor = target_gain;

    float cur[2], nxt[2];
    uint8_t out_l[8], out_r[8];
    uint64_t frame_count = 0;

    if (!std::cin.read(reinterpret_cast<char*>(cur), 8)) return 0;

    // 清屏并隐藏光标
    std::cerr << "\033[2J\033[H\033[?25l";

    while (std::cin.read(reinterpret_cast<char*>(nxt), 8)) {
        for (int i = 0; i < 8; ++i) {
            uint8_t bl = 0, br = 0;
            for (int bit = 7; bit >= 0; --bit) {
                float alpha = static_cast<float>(i * 8 + (7 - bit)) / 64.0f;
                // 线性插值并调制
                if (mod_l.modulate(cur[0]*(1.0-alpha) + nxt[0]*alpha)) bl |= (1 << bit);
                if (mod_r.modulate(cur[1]*(1.0-alpha) + nxt[1]*alpha)) br |= (1 << bit);
            }
            out_l[i] = bl; out_r[i] = br;
        }

        // 高效二进制输出
        std::cout.write(reinterpret_cast<char*>(&out_l[0]), 4);
        std::cout.write(reinterpret_cast<char*>(&out_r[0]), 4);
        std::cout.write(reinterpret_cast<char*>(&out_l[4]), 4);
        std::cout.write(reinterpret_cast<char*>(&out_r[4]), 4);

        cur[0] = nxt[0]; cur[1] = nxt[1];
        frame_count++;

        // 异步 UI 刷新：每秒约 10 次 (384000 / 10 = 38400)
        if (frame_count % 38400 == 0) {
            std::cerr << "\033[H"; // 光标回顶部
            auto render = [&](const char* name, SDM5& m) {
                // 仅在显示时才进行复杂的对数计算 📉
                double db = 20.0 * std::log10(std::abs(m.pcm_sample) + 1e-9);
                double pct = (m.total_samples > 0) ? (double)m.s4_clip_hits / m.total_samples * 100.0 : 0.0;
                
                std::cerr << name << " | PCM: " << std::fixed << std::setprecision(1) << std::setw(5) << db << " dB"
                          << " | S4: " << std::setw(4) << (int)m.current_s4
                          << " | CLIP(S4): " << std::setw(6) << m.s4_clip_hits 
                          << " (" << pct << "%)\n";
                m.reset_metrics();
            };

            std::cerr << "\033[1;36m>>> DSD512 ULTIMATE ENGINE | GAIN: " << target_gain << " <<<\033[0m\n";
            render("L", mod_l);
            render("R", mod_r);
            std::cerr << std::flush;
        }
    }
    
    std::cerr << "\033[?25h"; // 恢复光标
    return 0;
}