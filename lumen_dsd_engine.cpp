#include <iostream>
#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <string>
#include <iomanip>
#include <unistd.h>

// 针对 ARM 缓存行对齐，减少 Pi 5 内存访问冲突
struct alignas(64) SDM5 {
    double s[5] = {0,0,0,0,0};
    double s_peak[5] = {0,0,0,0,0};
    double s_scale[5] = {100.0, 100.0, 100.0, 100.0, 100.0}; 
    uint64_t s_clips[5] = {0,0,0,0,0}; 
    double q = 0;
    const double LIMIT = 100.0;
    double gain_factor = 0.8;
    
    double max_stress_period = 0;
    double global_stress_scale = 100.0; 
    uint64_t total_samples = 0;
    uint64_t total_clips = 0;

    void reset() {
        for(int i=0; i<5; ++i) { 
            s[i] = 0; s_peak[i] = 0; s_clips[i] = 0; 
            s_scale[i] = 100.0; 
        }
        q = 0; total_samples = 0; total_clips = 0; max_stress_period = 0;
        global_stress_scale = 100.0;
    }

    // Pi 5 优化：手动内联和分支预测优化
    inline int modulate(const double& input) {
        const double x = input * gain_factor;
        total_samples++;
        
        // 5阶积分器核心 - 展开循环以利用 Pi 5 的流水线执行
        s[0] += (x - q);
        s[1] += (s[0] - q * 0.62);
        s[2] += (s[1] - q * 0.16);
        s[3] += (s[2] - q * 0.025);
        s[4] += (s[3] - q * 0.0015);

        bool clipped = false;
        // 使用编译器能识别的定长循环，方便开启 SIMD 优化
        #pragma GCC ivdep
        for (int i = 0; i < 5; ++i) {
            double a = std::abs(s[i]);
            if (a > s_peak[i]) s_peak[i] = a;
            if (a > max_stress_period) max_stress_period = a;
            
            if (a >= LIMIT) {
                s_clips[i]++;
                s[i] = (s[i] > 0) ? LIMIT : -LIMIT;
                clipped = true;
            }
        }
        
        if (clipped) total_clips++;
        
        // 量化器
        q = (s[4] >= 0) ? 1.0 : -1.0;
        return (q > 0) ? 1 : 0;
    }
};

// UI 渲染函数 - 减少终端刷新带来的系统调用负担
std::string draw_auto_bar(std::string lab, double val, double &scale, std::string color, int width = 30) {
    if (val > scale) scale = val * 1.05;
    else if (scale > 100.0) scale *= 0.992; 
    if (scale < 100.0) scale = 100.0;

    float pct = (float)std::min(1.0, val / scale);
    int filled = static_cast<int>(pct * width);
    std::string res = lab + " [" + color;
    for (int i = 0; i < width; ++i) {
        if (i < filled) res += "#";
        else if (i == filled) res += ">";
        else res += "-";
    }
    res += "\033[0m] ";
    
    char info[64];
    snprintf(info, sizeof(info), "%4d/%-4d", (int)val, (int)scale);
    return res + info;
}

int main(int argc, char* argv[]) {
    double target_gain = (argc > 1) ? std::atof(argv[1]) : 0.8;
    SDM5 mod_l, mod_r;
    mod_l.gain_factor = mod_r.gain_factor = target_gain;

    float cur[2], nxt[2];
    uint64_t total_frames = 0;
    double p_l = 0, p_r = 0, pcm_scale = 60.0;

    // 预分配 buffer 提高写入效率
    std::vector<char> write_buf(16);

    std::cerr << "\033[2J\033[H\033[?25l"; 

    while (std::cin.read(reinterpret_cast<char*>(cur), 8)) {
        if (!std::cin.read(reinterpret_cast<char*>(nxt), 8)) break;
        
        p_l = std::max(p_l, (double)std::abs(cur[0]));
        p_r = std::max(p_r, (double)std::abs(cur[1]));

        uint8_t out[16]; 
        // DSD512 插值算法优化：针对 Pi 5 的单指令流多数据友好排布
        for (int i = 0; i < 8; ++i) {
            uint8_t bl = 0, br = 0;
            for (int bit = 7; bit >= 0; --bit) {
                float alpha = static_cast<float>(i * 8 + (7 - bit)) / 64.0f;
                if (mod_l.modulate(cur[0] * (1.0f - alpha) + nxt[0] * alpha)) bl |= (1 << bit);
                if (mod_r.modulate(cur[1] * (1.0f - alpha) + nxt[1] * alpha)) br |= (1 << bit);
            }
            out[i] = bl; out[i + 8] = br;
        }

        if (!isatty(STDOUT_FILENO)) {
            // Pi 5 的 I/O 缓冲区交替写入，保证 DSD 声道对齐
            std::cout.write(reinterpret_cast<char*>(&out[0]), 4); 
            std::cout.write(reinterpret_cast<char*>(&out[8]), 4);
            std::cout.write(reinterpret_cast<char*>(&out[4]), 4); 
            std::cout.write(reinterpret_cast<char*>(&out[12]), 4);
        }

        // 刷新频率锁定在 0.5 秒，避免终端 I/O 抢占 CPU 时间
        if (++total_frames % 176400 == 0) {
            std::cerr << "\033[H\033[1;32m>>> PI-5 DSD512 OPTIMIZED | ADAPTIVE ANALYZER | GAIN: " << target_gain << " <<<\033[0m\n\n";

            auto render_ch = [&](std::string name, double pk, SDM5& m) {
                double db = (pk < 1e-7) ? -60.0 : 20.0 * std::log10(pk);
                double total_clip_pct = (m.total_samples > 0) ? (double)m.total_clips / m.total_samples * 100.0 : 0.0;

                std::cerr << "\033[1;37m[" << name << " CHANNEL]\033[0m\n";
                std::cerr << draw_auto_bar("  PCM   ", db + 60.0, pcm_scale, "\033[1;32m") << " dB\n";
                std::cerr << draw_auto_bar("  STRESS", m.max_stress_period, m.global_stress_scale, "\033[1;31m") 
                          << " (" << std::fixed << std::setprecision(2) << total_clip_pct << "% CLIP)\n";
                
                for(int i = 0; i < 5; ++i) {
                    float s_clip_pct = (m.total_samples > 0) ? (float)m.s_clips[i] / m.total_samples * 100.0f : 0.0f;
                    std::string s_col = (s_clip_pct > 1.0) ? "\033[1;31m" : (s_clip_pct > 0.01 ? "\033[1;33m" : "\033[1;34m");
                    std::cerr << draw_auto_bar("  Stage " + std::to_string(i), m.s_peak[i], m.s_scale[i], s_col, 22);
                    std::cerr << " \033[0;30m(" << std::fixed << std::setprecision(2) << s_clip_pct << "%)\033[0m\n";
                }
                std::cerr << "\n";
                
                for(int i = 0; i < 5; ++i) { m.s_peak[i] = 0; m.s_clips[i] = 0; }
                m.total_samples = 0; m.total_clips = 0; m.max_stress_period = 0;
            };

            render_ch("LEFT", p_l, mod_l);
            render_ch("RIGHT", p_r, mod_r);
            std::cerr << std::flush;
        }
    }
    std::cerr << "\033[?25h";
    return 0;
}