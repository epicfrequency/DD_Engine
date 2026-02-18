#include <iostream>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <string>
#include <cstdio>

// --- LUMEN DSD512 核心调制引擎 ---
class SDM5 {
private:
    double s[5] = {0, 0, 0, 0, 0};
    double q = 0;
    uint64_t total_samples = 0;
    uint64_t clip_samples = 0;

public:
    double max_stress = 0;
    double max_clip_ever = 0;

    double get_clip_rate() {
        if (total_samples == 0) return 0.0;
        double rate = (double)clip_samples / total_samples * 100.0;
        if (rate > max_clip_ever) max_clip_ever = rate; 
        total_samples = 0;
        clip_samples = 0;
        return rate;
    }

inline int modulate(double x, bool is_licensed) {
    total_samples++;
    
    // 输入限幅（继续收紧，直到系统稳定）
    if (x > 0.5) x = 0.5; 
    if (x < -0.5) x = -0.5;

    // --- 增强型二阶结构 ---
    // 我们给反馈 q 乘上一个系数（比如 0.5），减小它对积分器的单次冲击
    // 同时稍微给积分增加一点衰减（0.99）
    s[0] = s[0] * 0.99 + (x - q * 0.4); 
    s[1] = s[1] * 0.99 + (s[0] - q * 0.2);

    bool has_clipped = false;
    double limit = is_licensed ? 128.0 : 2.0; 
    
    for(int i = 0; i < 2; ++i) {
        if (s[i] > limit) { s[i] = limit; has_clipped = true; }
        else if (s[i] < -limit) { s[i] = -limit; has_clipped = true; }
    }
    
    if (has_clipped) clip_samples++;
    
    // 更新指标
    double cur_stress = std::abs(s[1]);
    if (cur_stress > max_stress) max_stress = cur_stress;

    // 量化：只要 s[1] 有正负倾向就输出 bit
    q = (s[1] >= 0) ? 1.0 : -1.0;
    return (s[1] >= 0) ? 1 : 0;
}
    
    void reset_metrics() { max_stress = 0; }
};

// --- UI 渲染系统 ---
class SystemManager {
public:
    static void draw(double gain, uint32_t sr, float db_l, float db_r, SDM5& sL, SDM5& sR, bool licensed) {
        double clp_l = sL.get_clip_rate();
        double clp_r = sR.get_clip_rate();

        fprintf(stderr, "\033[H");
        fprintf(stderr, "\033[1;36mLUMEN DSD512 | GAIN: %.2f | SR: %u Hz | LICENSE: %s\033[0m\033[K\n\n", 
                gain, sr, licensed ? "\033[1;32mACTIVE\033[0m" : "\033[1;31mUNPAID\033[0m");

        auto row = [](const char* label, float db, double stress, double clip, double max_c) {
            int vol_w = std::max(0, std::min(40, (int)((db + 60.0) / 60.0 * 40.0)));
            std::string vol_bar = std::string(vol_w, '#') + std::string(40 - vol_w, '-');
            int str_w = std::max(0, std::min(40, (int)(stress / 128.0 * 40.0)));
            std::string str_bar = std::string(str_w, '#') + std::string(40 - str_w, ' ');

            const char* c_color = (clip > 0.5) ? "\033[1;31m" : "\033[1;32m";
            fprintf(stderr, "%s-LEVEL  [%s] %6.1f dB\033[K\n", label, vol_bar.c_str(), db);
            fprintf(stderr, "%s-STRESS [%s] %6.1f %s(CLIP: %5.2f%%)\033[0m | MAX: %5.2f%%\033[K\n", 
                    label, str_bar.c_str(), stress, c_color, clip, max_c);
        };

        row("L", db_l, sL.max_stress, clp_l, sL.max_clip_ever);
        row("R", db_r, sR.max_stress, clp_r, sR.max_clip_ever);
    }
};

int main(int argc, char** argv) {
    double gain = (argc > 1) ? std::atof(argv[1]) : 0.35;
    uint32_t sample_rate = (argc > 2) ? (uint32_t)std::stoul(argv[2]) : 384000;
    
    bool licensed = true; 
    const uint32_t UPDATE_INTERVAL = sample_rate / 4; 

    SDM5 sdmL, sdmR;
    float buffer[2];
    uint64_t count = 0;

    fprintf(stderr, "\033[2J"); 

    while (fread(buffer, sizeof(float), 2, stdin) == 2) {
        sdmL.modulate(buffer[0] * gain, licensed);
        sdmR.modulate(buffer[1] * gain, licensed);

        if (++count >= UPDATE_INTERVAL) {
            float dbl = 20.0f * std::log10(std::abs(buffer[0]) + 1e-6f);
            float dbr = 20.0f * std::log10(std::abs(buffer[1]) + 1e-6f);
            SystemManager::draw(gain, sample_rate, dbl, dbr, sdmL, sdmR, licensed);
            sdmL.reset_metrics(); sdmR.reset_metrics();
            count = 0;
        }
    }
    return 0;
}