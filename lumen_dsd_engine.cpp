#include <iostream>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <cstdio>

// --- 核心调制引擎 ---
class SDM5 {
private:
    double s[5] = {0,0,0,0,0};
    double q = 0;
    const double LIMIT = 100.0;
    
public:
    double gain_factor = 0.5;
    double max_stress = 0;

    // 强制内联以保持 DSD512 的实时性
    inline int modulate(double x) {
        // 5阶积分链展开
        s[0] += (x - q);
        s[1] += (s[0] - q * 0.5);
        s[2] += (s[1] - q * 0.25);
        s[3] += (s[2] - q * 0.125);
        s[4] += (s[3] - q * 0.0625);

        double cur_s4 = (s[4] >= 0) ? s[4] : -s[4];
        if (cur_s4 > max_stress) max_stress = cur_s4;

        // 溢出保护
        if (cur_s4 >= LIMIT) {
            s[4] = (s[4] > 0) ? LIMIT : -LIMIT;
        }
        
        // 自动复位逻辑：如果彻底失控（417崩溃），尝试强制清零
        if (cur_s4 > 1000.0) {
            for(int i=0; i<5; ++i) s[i] = 0;
            q = 0;
        }

        int bit = (s[4] >= 0) ? 1 : 0;
        q = bit ? 1.0 : -1.0;
        return bit;
    }

    void reset_metrics() { max_stress = 0; }
};

// --- 零延迟渲染引擎 ---
class Visualizer {
public:
    static void init_screen() {
        printf("\033[2J\033[H\033[?25l"); // 清屏并隐藏光标
    }

    static void draw_frame(double gain, float db_l, float db_r, double stress_l, double stress_r) {
        fprintf(stderr, "\033[H\033[1;36mLUMEN DSD512 ENGINE | GAIN: %.2f\033[0m\n\n", gain);
        render_bar("LEFT  LEVEL", db_l, -60.0f, 0.0f, "\033[1;32m");
        render_bar("LEFT  STRESS", stress_l, 0.0f, 120.0f, "\033[1;31m");
        fprintf(stderr, "\n");
        render_bar("RIGHT LEVEL", db_r, -60.0f, 0.0f, "\033[1;32m");
        render_bar("RIGHT STRESS", stress_r, 0.0f, 120.0f, "\033[1;31m");
        fflush(stderr);
    }

private:
    static void render_bar(const char* label, float val, float min_v, float max_v, const char* color) {
        const int width = 40;
        float range = max_v - min_v;
        int filled = static_cast<int>(((val - min_v) / range) * width);
        filled = std::max(0, std::min(width, filled));

        fprintf(stderr, "%s %s[", label, color);
        for (int i = 0; i < width; ++i) fputc((i < filled) ? '#' : '-', stderr);
        fprintf(stderr, "] %.1f\033[0m\n", val);
    }
};

// --- 主音频处理器 ---
class AudioProcessor {
private:
    SDM5 mod_l, mod_r;
    float peak_l = 0, peak_r = 0;
    uint64_t frame_count = 0;

public:
    void run(double target_gain) {
        mod_l.gain_factor = target_gain;
        mod_r.gain_factor = target_gain;

        float cur[2], nxt[2];
        Visualizer::init_screen();

        // 预设 stdout 缓冲区，降低系统调用频率
        static char out_buffer[65536];
        std::setvbuf(stdout, out_buffer, _IOFBF, sizeof(out_buffer));

        if (!std::cin.read(reinterpret_cast<char*>(cur), 8)) return;

        while (std::cin.read(reinterpret_cast<char*>(nxt), 8)) {
            process_pair(cur, nxt);
            cur[0] = nxt[0]; cur[1] = nxt[1];
            
            if (++frame_count % 44100 == 0) {
                update_ui(target_gain);
            }
        }
    }

private:
    void process_pair(float* cur, float* nxt) {
        peak_l = std::max(peak_l, std::abs(cur[0]));
        peak_r = std::max(peak_r, std::abs(cur[1]));

        uint8_t l_bits[8], r_bits[8];
        
        // 线性插值 + 调制 (8字节打包循环)
        for (int i = 0; i < 8; ++i) {
            uint8_t bl = 0, br = 0;
            for (int bit = 7; bit >= 0; --bit) {
                float alpha = (i * 8 + (7 - bit)) / 64.0f;
                if (mod_l.modulate(cur[0] * (1.0f - alpha) + nxt[0] * alpha)) bl |= (1 << bit);
                if (mod_r.modulate(cur[1] * (1.0f - alpha) + nxt[1] * alpha)) br |= (1 << bit);
            }
            l_bits[i] = bl; r_bits[i] = br;
        }

        // 符合 DSD_U32_BE 的管道输出
        std::cout.write(reinterpret_cast<char*>(&l_bits[0]), 4);
        std::cout.write(reinterpret_cast<char*>(&r_bits[0]), 4);
        std::cout.write(reinterpret_cast<char*>(&l_bits[4]), 4);
        std::cout.write(reinterpret_cast<char*>(&r_bits[4]), 4);
    }

    void update_ui(double gain) {
        float db_l = 20.0f * std::log10(peak_l + 1e-9f);
        float db_r = 20.0f * std::log10(peak_r + 1e-9f);
        Visualizer::draw_frame(gain, db_l, db_r, mod_l.max_stress, mod_r.max_stress);
        
        peak_l = 0; peak_r = 0;
        mod_l.reset_metrics();
        mod_r.reset_metrics();
    }
};

int main(int argc, char* argv[]) {
    double gain = (argc > 1) ? std::atof(argv[1]) : 0.315; // 默认 -10dB 保护级别
    AudioProcessor engine;
    engine.run(gain);
    return 0;
}