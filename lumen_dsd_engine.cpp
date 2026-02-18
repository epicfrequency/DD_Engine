#include <iostream>
#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <string>
#include <iomanip>
#include <unistd.h>

struct alignas(64) SDM5 {
    double s[5] = {0,0,0,0,0};
    double s_peak[5] = {0,0,0,0,0}; 
    double q = 0;
    const double LIMIT = 100.0;
    double gain_factor = 0.5;
    
    double max_stress = 0;
    uint64_t samples_count = 0;
    uint64_t clip_count = 0;

    // --- 恢复上一版原始音频逻辑 ---
    inline int modulate(double input) {
        const double x = input * gain_factor;
        samples_count++;
        
        s[0] += (x - q);
        s[1] += (s[0] - q * 0.5);
        s[2] += (s[1] - q * 0.25);
        s[3] += (s[2] - q * 0.125);
        s[4] += (s[3] - q * 0.0625);

        bool clipped = false;
        for (int i = 0; i < 5; ++i) {
            double abs_s = std::abs(s[i]);
            if (abs_s > s_peak[i]) s_peak[i] = abs_s; 
            if (abs_s > max_stress) max_stress = abs_s;
            if (abs_s >= LIMIT) {
                s[i] = (s[i] > 0) ? LIMIT : -LIMIT;
                clipped = true;
            }
        }
        if (clipped) clip_count++;
        
        int bit = (s[4] >= 0) ? 1 : 0;
        q = bit ? 1.0 : -1.0;
        return bit;
    }
};

// 🎨 独立量程渲染函数：20Hz 模式下保持高效
std::string make_dynamic_bar(std::string lab, double val, std::string default_color) {
    const int width = 30;
    double range = std::max(100.0, val); // 自动调整量程
    int filled = static_cast<int>((val / range) * width);
    
    std::string color = (val >= 100.0) ? "\033[1;31m" : default_color;
    
    std::string res = lab + " [" + color;
    for (int i = 0; i < width; ++i) res += (i < filled) ? "#" : "-";
    res += "\033[0m] " + color + std::to_string((int)val) + "\033[0m";
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

        // --- UI 刷新：384,000 / 20 = 19200 ---
        if (total_frames % 19200 == 0) {
            std::cerr << "\033[H\033[1;36m>>> LUMEN DSD512 MONITOR | 20Hz | GAIN: " << target_gain << " <<<\033[0m\n\n";
            auto render = [&](std::string name, float p, SDM5& m) {
                double db = (p < 1e-7) ? -60.0 : 20.0 * std::log10(p);
                double c_rate = (m.samples_count > 0) ? (double)m.clip_count / m.samples_count * 100.0 : 0.0;
                
                std::cerr << "\033[1;37m[" << name << "]\033[0m | CLIP: " 
                          << (c_rate > 0 ? "\033[1;31m" : "\033[1;32m") 
                          << std::fixed << std::setprecision(4) << c_rate << "%\033[0m\n";
                
                // S0-S4 独立 Bar 监控
                for(int i=0; i<5; ++i) {
                    std::string s_name = "  S" + std::to_string(i) + "    ";
                    std::cerr << make_dynamic_bar(s_name, m.s_peak[i], "\033[1;34m") << "\n";
                    m.s_peak[i] = 0; 
                }
                std::cerr << "\n";
                m.max_stress = 0; m.samples_count = 0; m.clip_count = 0;
            };
            render("LEFT ", peak_l, mod_l);
            render("RIGHT", peak_r, mod_r);
            peak_l = 0; peak_r = 0;
            std::cerr << std::flush;
        }
    }
    return 0;
}