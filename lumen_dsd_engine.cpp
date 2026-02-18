#include <iostream>
#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <iomanip>

struct alignas(64) SDM5 {
    double s[5] = {0,0,0,0,0};
    double q = 0;
    const double LIMIT = 128.0;
    double gain_factor = 0.5;

    // 极致监控：仅保留核心计数和瞬时采样
    uint64_t s4_clip_hits = 0; 
    double current_s4 = 0;
    double pcm_sample = 0;

    inline int modulate(double input) {
        const double x = input * gain_factor;

        // 1. 核心调制逻辑
        s[0] += (x - q);
        s[1] += (s[0] - q * 0.5);
        s[2] += (s[1] - q * 0.25);
        s[3] += (s[2] - q * 0.125);
        s[4] += (s[3] - q * 0.0625); // 试试看S4反馈增强一点

        // 2. 前四阶仅做 Clamp
        for (int i = 0; i < 4; ++i) {
            if (s[i] > LIMIT) s[i] = LIMIT;
            else if (s[i] < -LIMIT) s[i] = -LIMIT;
        }

        // 3. 仅对 S4 进行截断计数
        if (s[4] > LIMIT) { s[4] = LIMIT; s4_clip_hits++; }
        else if (s[4] < -LIMIT) { s[4] = -LIMIT; s4_clip_hits++; }

        // 采样瞬时值
        current_s4 = s[4];
        pcm_sample = x;

        int bit = (s[4] >= 0) ? 1 : 0;
        q = bit ? 1.0 : -1.0;
        return bit;
    }

    void reset_metrics() {
        s4_clip_hits = 0;
    }
};

int main(int argc, char* argv[]) {
    double target_gain = (argc > 1) ? std::atof(argv[1]) : 0.5;
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(NULL);

    SDM5 mod_l, mod_r;
    mod_l.gain_factor = mod_r.gain_factor = target_gain;

    float cur[2], nxt[2];
    uint8_t out_l[8], out_r[8];
    uint64_t frame_count = 0;

    if (!std::cin.read(reinterpret_cast<char*>(cur), 8)) return 0;
    std::cerr << "\033[2J\033[H\033[?25l";

    while (std::cin.read(reinterpret_cast<char*>(nxt), 8)) {
        for (int i = 0; i < 8; ++i) {
            uint8_t bl = 0, br = 0;
            for (int bit = 7; bit >= 0; --bit) {
                float alpha = static_cast<float>(i * 8 + (7 - bit)) / 64.0f;
                if (mod_l.modulate(cur[0]*(1.0-alpha) + nxt[0]*alpha)) bl |= (1 << bit);
                if (mod_r.modulate(cur[1]*(1.0-alpha) + nxt[1]*alpha)) br |= (1 << bit);
            }
            out_l[i] = bl; out_r[i] = br;
        }

        std::cout.write(reinterpret_cast<char*>(&out_l[0]), 4);
        std::cout.write(reinterpret_cast<char*>(&out_r[0]), 4);
        std::cout.write(reinterpret_cast<char*>(&out_l[4]), 4);
        std::cout.write(reinterpret_cast<char*>(&out_r[4]), 4);

        cur[0] = nxt[0]; cur[1] = nxt[1];
        frame_count++;

        // 利用 frame_count 推算百分比，每 0.1 秒刷新一次
        if (frame_count % 38400 == 0) {
            std::cerr << "\033[H"; 
            auto render = [&](const char* name, SDM5& m) {
                double db = 20.0 * std::log10(std::abs(m.pcm_sample) + 1e-9);
                // 直接用 38400 帧 * 64 倍插值得到该时段总样本数
                double pct = (double)m.s4_clip_hits / (38400.0 * 64.0) * 100.0;
                
                std::cerr << name << " | PCM: " << std::fixed << std::setprecision(1) << std::setw(5) << db << " dB"
                          << " | S4: " << std::setw(4) << (int)m.current_s4
                          << " | CLIP(S4): " << std::setw(6) << m.s4_clip_hits 
                          << " (" << std::setprecision(3) << pct << "%)\n";
                m.reset_metrics();
            };
            std::cerr << "--- DSD512 MINIMAL MONITOR ---\n";
            render("L", mod_l);
            render("R", mod_r);
            std::cerr << std::flush;
        }
    }
    std::cerr << "\033[?25h";
    return 0;
}