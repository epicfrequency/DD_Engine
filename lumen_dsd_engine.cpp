#include <iostream>
#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <string>
#include <iomanip>
#include <unistd.h>

// 🔬 调制器结构：包含层级监控
struct alignas(64) SDM5 {
    double s[5] = {0,0,0,0,0};
    double s_peak[5] = {0,0,0,0,0};  // 记录 S0-S4 的峰值 📈
    double q = 0;
    const double LIMIT = 100.0;
    double gain_factor = 0.5;
    
    double max_stress_period = 0;
    uint64_t total_samples = 0;
    uint64_t total_clips = 0;

    void reset() {
        for(int i=0; i<5; ++i) { s[i]=0; s_peak[i]=0; }
        q = 0; total_samples = 0; total_clips = 0; max_stress_period = 0;
    }

    inline int modulate(double input) {
        const double x = input * gain_factor;
        total_samples++;
        
        // 5阶积分核心逻辑 
        s[0] += (x - q);
        s[1] += (s[0] - q * 0.5);
        s[2] += (s[1] - q * 0.25);
        s[3] += (s[2] - q * 0.125);
        s[4] += (s[3] - q * 0.0625);

        bool has_clipped = false;
        for (int i = 0; i < 5; ++i) {
            double abs_s = std::abs(s[i]);
            if (abs_s > s_peak[i]) s_peak[i] = abs_s;
            if (abs_s > max_stress_period) max_stress_period = abs_s;
            
            if (abs_s >= LIMIT) {
                s[i] = (s[i] > 0) ? LIMIT : -LIMIT;
                has_clipped = true;
            }
        }
        if (has_clipped) total_clips++;
        
        int bit = (s[4] >= 0) ? 1 : 0;
        q = bit ? 1.0 : -1.0;
        return bit;
    }
};

// 🎨 渲染工具
std::string make_bar(std::string lab, double val, double max_v, std::string color, int width = 30) {
    int filled = static_cast<int>((std::min(val, max_v) / max_v) * width);
    std::string res = lab + " [" + color;
    for (int i = 0; i < width; ++i) res += (i < filled) ? "#" : "-";
    res += "\033[0m] ";
    return res;
}

int main(int argc, char* argv[]) {
    double target_gain = (argc > 1) ? std::atof(argv[1]) : 0.5;
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(NULL);

    SDM5 mod_l, mod_r;
    mod_l.gain_factor = mod_r.gain_factor = target_gain;

    float cur[2], nxt[2];
    uint8_t out_l[8], out_r[8];
    uint64_t total_frames = 0;
    float peak_l = 0, peak_r = 0;

    if (!std::cin.read(reinterpret_cast<char*>(cur), 8)) return 0;
    std::cerr << "\033[2J\033[H\033[?25l"; 

    while (std::cin.read(reinterpret_cast<char*>(nxt), 8)) {
        peak_l = std::max(peak_l, std::abs(cur[0]));
        peak_r = std::max(peak_r, std::abs(cur[1]));

        for (int i = 0; i < 8; ++i) {
            uint8_t bl = 0, br = 0;
            for (int bit = 7; bit >= 0; --bit) {
                float alpha = static_cast<float>(i * 8 + (7 - bit)) / 64.0f;
                if (mod_l.modulate(cur[0]*(1.0f-alpha) + nxt[0]*alpha)) bl |= (1 << bit);
                if (mod_r.modulate(cur[1]*(1.0f-alpha) + nxt[1]*alpha)) br |= (1 << bit);
            }
            out_l[i] = bl; out_r[i] = br;
        }

        // 📤 输出 DSD512 数据块
        std::cout.write(reinterpret_cast<char*>(&out_l[0]), 4);
        std::cout.write(reinterpret_cast<char*>(&out_r[0]), 4);
        std::cout.write(reinterpret_cast<char*>(&out_l[4]), 4);
        std::cout.write(reinterpret_cast<char*>(&out_r[4]), 4);

        cur[0] = nxt[0]; cur[1] = nxt[1];
        total_frames++;

        // ⏱️ 1秒渲染4次 (44100 / 4 ≈ 11025)
        if (total_frames % 11025 == 0) {
            std::cerr << "\033[H\033[1;36m--- LUMEN DSD512 ENGINE | GAIN: " << target_gain << " ---\033[0m\n\n";

            auto render_ch = [&](std::string name, float pk, SDM5& m) {
                double db = (pk < 1e-7) ? -60.0 : 20.0 * std::log10(pk);
                double clip_rate = (double)m.total_clips / m.total_samples * 100.0;

                std::cerr << "\033[1;37m" << name << "\033[0m\n";
                std::cerr << make_bar("  PCM   ", db + 60.0, 60.0, "\033[1;32m") << (int)db << " dB\n";
                std::cerr << make_bar("  STRESS", m.max_stress_period, 120.0, "\033[1;31m") << std::fixed << std::setprecision(3) << clip_rate << "% CLIP\n";
                
                std::cerr << "  LEVELS ";
                for(int i=0; i<5; ++i) {
                    std::string s_color = (m.s_peak[i] > 90) ? "\033[1;33m" : "\033[1;34m";
                    std::cerr << "S" << i << ":" << s_color << std::setw(3) << (int)m.s_peak[i] << " \033[0m";
                    m.s_peak[i] *= 0.5; // 留一点残影效果
                }
                std::cerr << "\n\n";
                m.total_samples = 0; m.total_clips = 0; m.max_stress_period = 0;
            };

            render_ch("LEFT CHANNEL", peak_l, mod_l);
            render_ch("RIGHT CHANNEL", peak_r, mod_r);
            peak_l = 0; peak_r = 0;
            std::cerr << std::flush;
        }
    }
    return 0;
}