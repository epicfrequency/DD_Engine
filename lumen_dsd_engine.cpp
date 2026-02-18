#include <iostream>
#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <string>
#include <iomanip>

struct alignas(64) SDM5 {
    double s[5] = {0,0,0,0,0};
    double s_peak[5] = {0,0,0,0,0}; 
    uint64_t s_clip_count[5] = {0,0,0,0,0}; // 🚀 每一阶独立的截断计数
    double q = 0;
    const double LIMIT = 100.0;
    double gain_factor = 0.1; 
    
    uint64_t interval_samples = 0; 

    inline int modulate(double input) {
        const double x = input * gain_factor;
        interval_samples++;
        
        // --- 核心五阶调制逻辑 (严禁变动) ---
        s[0] += (x - q);
        s[1] += (s[0] - q * 0.5);
        s[2] += (s[1] - q * 0.25);
        s[3] += (s[2] - q * 0.125);
        s[4] += (s[3] - q * 0.0625);

        for (int i = 0; i < 5; ++i) {
            double abs_s = std::abs(s[i]);
            if (abs_s > s_peak[i]) s_peak[i] = abs_s; 
            
            if (abs_s >= LIMIT) {
                s[i] = (s[i] > 0) ? LIMIT : -LIMIT;
                s_clip_count[i]++; // 🚀 记录该阶截断
            }
        }
        
        int bit = (s[4] >= 0) ? 1 : 0;
        q = bit ? 1.0 : -1.0;
        return bit;
    }

    void reset_metrics() {
        for(int i=0; i<5; ++i) {
            s_peak[i] = 0;
            s_clip_count[i] = 0;
        }
        interval_samples = 0;
    }
};

// 🎨 独立量程渲染函数
std::string make_independent_bar(std::string lab, double val, double clip_pct) {
    const int width = 30;
    // 自动量程：基准 100，若溢出则以当前值为准
    double range = std::max(100.0, val);
    int filled = static_cast<int>((val / range) * width);
    
    // 超过 100 变红，否则蓝色
    std::string color = (val >= 100.0) ? "\033[1;31m" : "\033[1;34m";
    std::string clip_color = (clip_pct > 0) ? "\033[1;31m" : "\033[1;32m";
    
    std::string res = lab + " [" + color;
    for (int i = 0; i < width; ++i) res += (i < filled) ? "#" : "-";
    res += "\033[0m] " + color + std::to_string((int)val) + "\033[0m";
    
    // 附加独立 Clip 百分比
    res += " | CLIP: " + clip_color + std::to_string((int)clip_pct) + "%\033[0m";
    return res;
}

int main(int argc, char* argv[]) {
    double target_gain = (argc > 1) ? std::atof(argv[1]) : 0.1;
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
        total_frames++;

        // --- UI 刷新：384,000 / 20 = 19,200 ---
        if (total_frames % 19200 == 0) {
            std::cerr << "\033[H\033[1;36m>>> LUMEN DSD512 MONITOR | 20Hz | GAIN: " << target_gain << " <<<\033[0m\n\n";
            auto render = [&](std::string name, float p, SDM5& m) {
                std::cerr << "\033[1;37m[" << name << "]\033[0m\n";
                for(int i=0; i<5; ++i) {
                    double c_pct = (m.interval_samples > 0) ? (double)m.s_clip_count[i] / m.interval_samples * 100.0 : 0.0;
                    std::cerr << make_independent_bar("  S" + std::to_string(i) + " ", m.s_peak[i], c_pct) << "\n";
                }
                std::cerr << "\n";
                m.reset_metrics();
            };
            render("LEFT ", peak_l, mod_l);
            render("RIGHT", peak_r, mod_r);
            peak_l = 0; peak_r = 0;
            std::cerr << std::flush;
        }
    }
    return 0;
}