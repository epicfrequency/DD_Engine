#include <iostream>
#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <string>
#include <iomanip>
#include <unistd.h>

// 5阶 Sigma-Delta 调制器结构体
struct SDM5 {
    double s[5] = {0,0,0,0,0};          // 积分器状态
    double s_peak[5] = {0,0,0,0,0};     // 周期内每一级的峰值
    double s_scale[5] = {100.0, 100.0, 100.0, 100.0, 100.0}; // 每一级独立的自动量程
    uint64_t s_clips[5] = {0,0,0,0,0};  // 每一级独立的 Clip 计数
    double q = 0;                       // 量化器反馈
    const double LIMIT = 100.0;         // 默认硬限幅
    double gain_factor = 0.8;           // 输入增益
    
    double max_stress_period = 0;       // 周期内系统最大压力
    double global_stress_scale = 100.0; // 总 Stress 的自动量程
    uint64_t total_samples = 0;         // 周期内总样本数
    uint64_t total_clips = 0;           // 周期内总 Clip 次数

    // 算法复位（逃逸保护）
    void reset() {
        for(int i=0; i<5; ++i) { 
            s[i] = 0; s_peak[i] = 0; s_clips[i] = 0; 
            s_scale[i] = 100.0; 
        }
        q = 0; total_samples = 0; total_clips = 0; max_stress_period = 0;
        global_stress_scale = 100.0;
    }

    // 核心调制逻辑
    inline int modulate(double input) {
        double x = input * gain_factor;
        total_samples++;
        
        // 优化后的系数分布：增强稳定性，防止 S4 能量堆积
        s[0] += (x - q);
        s[1] += (s[0] - q * 0.62);
        s[2] += (s[1] - q * 0.16);
        s[3] += (s[2] - q * 0.025);
        s[4] += (s[3] - q * 0.0015);

        bool any_clip = false;
        for (int i = 0; i < 5; ++i) {
            double abs_val = std::abs(s[i]);
            if (abs_val > s_peak[i]) s_peak[i] = abs_val;
            if (abs_val > max_stress_period) max_stress_period = abs_val;
            
            // 每一级截断检查
            if (abs_val >= LIMIT) {
                s_clips[i]++;
                s[i] = (s[i] > 0) ? LIMIT : -LIMIT;
                any_clip = true;
            }
        }
        
        if (any_clip) total_clips++;
        
        // 逃逸自救：如果末级数值大得离谱，强制重置
        if (std::abs(s[4]) > 5000.0) reset();

        // 1-bit 量化
        int bit = (s[4] >= 0) ? 1 : 0;
        q = bit ? 1.0 : -1.0;
        return bit;
    }
};

// 带 Auto-Scale 逻辑的进度条渲染
std::string draw_auto_bar(std::string lab, double val, double &scale, std::string color, int width = 35) {
    // 扩容快，缩回慢（Decay 效果）
    if (val > scale) scale = val * 1.05;
    else if (scale > 100.0) scale *= 0.99; 
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

    // 隐藏光标并清屏
    std::cerr << "\033[2J\033[H\033[?25l"; 

    while (std::cin.read(reinterpret_cast<char*>(cur), 8)) {
        if (!std::cin.read(reinterpret_cast<char*>(nxt), 8)) break;
        
        p_l = std::max(p_l, static_cast<double>(std::abs(cur[0])));
        p_r = std::max(p_r, static_cast<double>(std::abs(cur[1])));

        uint8_t out[16]; 
        // 64倍过采样插值 (DSD512)
        for (int i = 0; i < 8; ++i) {
            uint8_t bl = 0, br = 0;
            for (int bit = 7; bit >= 0; --bit) {
                float a = static_cast<float>(i * 8 + (7 - bit)) / 64.0f;
                double sample_l = cur[0] * (1.0f - a) + nxt[0] * a;
                double sample_r = cur[1] * (1.0f - a) + nxt[1] * a;
                
                if (mod_l.modulate(sample_l)) bl |= (1 << bit);
                if (mod_r.modulate(sample_r)) br |= (1 << bit);
            }
            out[i] = bl;     // Left 字节串
            out[i + 8] = br; // Right 字节串
        }

        // 非 TTY 环境输出二进制 DSD 流 (4-4-4-4 交织模式)
        if (!isatty(STDOUT_FILENO)) {
            std::cout.write(reinterpret_cast<char*>(&out[0]), 4);  // L low
            std::cout.write(reinterpret_cast<char*>(&out[8]), 4);  // R low
            std::cout.write(reinterpret_cast<char*>(&out[4]), 4);  // L high
            std::cout.write(reinterpret_cast<char*>(&out[12]), 4); // R high
        }

        // 每 0.5 秒刷新一次 UI (352800 / 2)
        if (++total_frames % 176400 == 0) {
            std::cerr << "\033[H\033[1;36m>>> LUMEN DSD512 | ADAPTIVE ANALYZER | GAIN: " << target_gain << " <<<\033[0m\n\n";

            auto render_channel = [&](std::string name, double pk, SDM5& m) {
                double db = (pk < 1e-7) ? -60.0 : 20.0 * std::log10(pk);
                double total_clip_pct = (m.total_samples > 0) ? (double)m.total_clips / m.total_samples * 100.0 : 0.0;

                std::cerr << "--- " << name << " CHANNEL ---\n";
                // 1. PCM 电平
                std::cerr << draw_auto_bar("  PCM   ", db + 60.0, pcm_scale, "\033[1;32m") << " dB\n";
                // 2. 总 Stress (带总 Clip 率)
                std::cerr << draw_auto_bar("  STRESS", m.max_stress_period, m.global_stress_scale, "\033[1;31m") 
                          << " (" << std::fixed << std::setprecision(2) << total_clip_pct << "% CLIP)\n";
                
                // 3. Stage 0-4 独立 Auto-Scale
                for(int i = 0; i < 5; ++i) {
                    float s_clip_pct = (m.total_samples > 0) ? static_cast<float>(m.s_clips[i]) / m.total_samples * 100.0f : 0.0f;
                    // 颜色逻辑：根据该级 Clip 率变色
                    std::string s_col = (s_clip_pct > 1.0f) ? "\033[1;31m" : (s_clip_pct > 0.01f ? "\033[1;33m" : "\033[1;34m");
                    
                    std::cerr << draw_auto_bar("  Stage " + std::to_string(i), m.s_peak[i], m.s_scale[i], s_col, 25);
                    std::cerr << " \033[0;30m(" << std::fixed << std::setprecision(2) << s_clip_pct << "%)\033[0m\n";
                }
                std::cerr << "\n";
                
                // 周期数据重置
                for(int i = 0; i < 5; ++i) { m.s_peak[i] = 0; m.s_clips[i] = 0; }
                m.total_samples = 0; m.total_clips = 0; m.max_stress_period = 0;
            };

            render_channel("LEFT ", p_l, mod_l);
            render_channel("RIGHT", p_r, mod_r);
            p_l = p_r = 0;
            std::cerr << std::flush;
        }
    }
    std::cerr << "\033[?25h"; // 恢复光标
    return 0;
}