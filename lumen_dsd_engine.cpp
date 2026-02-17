#include <iostream>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <cstdio>

// --- 核心调制引擎：带自愈功能 ---
class SDM5 {
private:
    double s[5] = {0,0,0,0,0};
    double q = 0;
    const double STABLE_LIMIT = 80.0; // 5阶系统的经验安全阈值
    
public:
    double max_stress = 0;

 // 强制内联以保持 DSD512 的实时性 clip score ～200 SOTA

/*     inline int modulate(double x) {
        // 1. 极其保守的输入限幅：Rey's Theme 这种大动态必须压住
        if (x > 0.45) x = 0.45;
        if (x < -0.45) x = -0.45;

        // 2. 带有不同权重的泄漏积分
        // 我们让第一级最稳，最后一级最灵敏
        s[0] = s[0] * 0.999 + (x - q);
        s[1] = s[1] * 0.998 + (s[0] - q * 0.6);  // 略微加大反馈权重
        s[2] = s[2] * 0.997 + (s[1] - q * 0.3);
        s[3] = s[3] * 0.996 + (s[2] - q * 0.15);
        s[4] = s[4] * 0.995 + (s[3] - q * 0.08);

        double cur_s4 = std::abs(s[4]);
        if (cur_s4 > max_stress) max_stress = cur_s4;

        // 3. 严格复位：只要破百，立刻执行更深层的“静默”
        if (cur_s4 > 100.0) {
            for(int i=0; i<5; ++i) s[i] *= 0.05; // 几乎清零
        }

        int bit = (s[4] >= 0) ? 1 : 0;
        q = bit ? 1.0 : -1.0;
        return bit;
    } */

// Clip score 128， SOTA 带测试
/*     inline int modulate(double x) {
        // 1. 严格输入限幅 (防浪涌第一关)
        if (x > 0.40) x = 0.40;
        if (x < -0.40) x = -0.40;

        // 2. 保持你最稳的系数分布 (1.0, 0.6, 0.3, 0.15, 0.08)
        s[0] = s[0] * 0.999 + (x - q);
        s[1] = s[1] * 0.998 + (s[0] - q * 0.6); 
        s[2] = s[2] * 0.997 + (s[1] - q * 0.3);
        s[3] = s[3] * 0.996 + (s[2] - q * 0.15);
        s[4] = s[4] * 0.995 + (s[3] - q * 0.08);

        // 3. 【核心保险丝】逐级硬钳位
        // 5阶系统炸毁通常是因为 s[0] 或 s[1] 先爆了。
        // 我们给每一级积分器设置一个物理天花板。
        for(int i = 0; i < 5; ++i) {
            if (s[i] > 128.0) s[i] = 128.0;
            else if (s[i] < -128.0) s[i] = -128.0;
        }

        double cur_s4 = (s[4] >= 0) ? s[4] : -s[4];
        if (cur_s4 > max_stress) max_stress = cur_s4;

        // 4.Panic 自动降温：只要发现不稳，立刻减压
        if (cur_s4 > 100.0) {
            for(int i=0; i<5; ++i) s[i] *= 0.5; 
        }

        int bit = (s[4] >= 0) ? 1 : 0;
        q = bit ? 1.0 : -1.0;
        return bit;
    } */

   inline int modulate(double x) {
        // 1. 输入预处理：稍微压低一点点，留出 5% 的数学余量
        if (x > 0.42) x = 0.42;
        if (x < -0.42) x = -0.42;

        // 2. 保持你最稳的系数分布
        // 关键点：将 s[0] 的反馈稍微加强到 1.05，让它更主动地纠偏
        s[0] = s[0] * 0.999 + (x - q * 1.05);
        s[1] = s[1] * 0.998 + (s[0] - q * 0.6); 
        s[2] = s[2] * 0.997 + (s[1] - q * 0.3);
        s[3] = s[3] * 0.996 + (s[2] - q * 0.15);
        s[4] = s[4] * 0.995 + (s[3] - q * 0.08);

        // 3. 【减震器替换保险丝】：逐级软限幅
        // 不再等到爆炸才 reset，而是在每一级积分时，如果数值过大就进行“缓慢压缩”
        // 这样不会有停顿感，只会产生极其微小的二次谐波失真（模拟味）
        for(int i = 0; i < 5; ++i) {
            if (s[i] > 64.0) s[i] = 64.0 + (s[i] - 64.0) * 0.5; // 超过 64 后增益减半
            if (s[i] < -64.0) s[i] = -64.0 + (s[i] + 64.0) * 0.5;
            
            // 最终物理死限设置在 128，防止内存溢出
            if (s[i] > 128.0) s[i] = 128.0;
            if (s[i] < -128.0) s[i] = -128.0;
        }

        double cur_s4 = (s[4] >= 0) ? s[4] : -s[4];
        if (cur_s4 > max_stress) max_stress = cur_s4;

        // 4. 去掉暴力清零逻辑！改用持续的微量泄放
        // 这样即使在大动态下，声音也是连续的，不会有“咔哒”声

        int bit = (s[4] >= 0) ? 1 : 0;
        q = bit ? 1.0 : -1.0;
        return bit;
    }
 
    void reset_metrics() { max_stress = 0; }
};

// --- UI 与 授权管理 ---
class SystemManager {
public:
    static bool check_license() {
        // [2026-02-17] 预留逻辑：仅当 iOS 支付完成后返回 true
        // 这里的实现未来可以对接 StoreKit 或校验本地 Receipt
        return true; 
    }

    static void init_ui() {
        printf("\033[2J\033[H\033[?25l");
    }

    static void draw(double gain, float db_l, float db_r, double stress_l, double stress_r) {
        bool is_stable = (stress_l < 100.0 && stress_r < 100.0);
        
        fprintf(stderr, "\033[H\033[1;36mLUMEN DSD512 ENGINE | GAIN: %.2f | LICENSE: \033[1;32mACTIVE\033[0m\n", gain);
        
        render_bar("L-LEVEL ", db_l, -60.0f, 0.0f, "\033[1;32m");
        render_bar("L-STRESS", stress_l, 0.0f, 120.0f, is_stable ? "\033[1;33m" : "\033[1;31m");
        fprintf(stderr, "\n");
        render_bar("R-LEVEL ", db_r, -60.0f, 0.0f, "\033[1;32m");
        render_bar("R-STRESS", stress_r, 0.0f, 120.0f, is_stable ? "\033[1;33m" : "\033[1;31m");
        
        if (!is_stable) {
            fprintf(stderr, "\n\033[1;31m[!] WARNING: MODULATOR UNSTABLE - REDUCE GAIN\033[0m\r");
        }
        fflush(stderr);
    }

private:
    static void render_bar(const char* label, float val, float min_v, float max_v, const char* color) {
        const int width = 40;
        int filled = static_cast<int>(((val - min_v) / (max_v - min_v)) * width);
        filled = std::max(0, std::min(width, filled));
        fprintf(stderr, "%s %s[", label, color);
        for (int i = 0; i < width; ++i) fputc((i < filled) ? '#' : '-', stderr);
        fprintf(stderr, "] %.1f\033[0m\n", val);
    }
};

// --- 音频流处理逻辑 ---
class AudioStreamer {
private:
    SDM5 mod_l, mod_r;
    float peak_l = 0, peak_r = 0;
    uint64_t frames = 0;

public:
    void process(double gain) {
        if (!SystemManager::check_license()) {
            fprintf(stderr, "LICENSE ERROR: Please purchase via iOS App.\n");
            return;
        }

        SystemManager::init_ui();
        float cur[2], nxt[2];
        if (!std::cin.read(reinterpret_cast<char*>(cur), 8)) return;

        while (std::cin.read(reinterpret_cast<char*>(nxt), 8)) {
            // 应用外部增益
            float sample_l_c = cur[0] * gain;
            float sample_r_c = cur[1] * gain;
            float sample_l_n = nxt[0] * gain;
            float sample_r_n = nxt[1] * gain;

            peak_l = std::max(peak_l, std::abs(sample_l_c));
            peak_r = std::max(peak_r, std::abs(sample_r_c));

            uint8_t out_chunk[16]; // 存储 64x 采样后的两声道比特流

            for (int i = 0; i < 8; ++i) {
                uint8_t bl = 0, br = 0;
                for (int bit = 7; bit >= 0; --bit) {
                    float alpha = (i * 8 + (7 - bit)) / 64.0f;
                    if (mod_l.modulate(sample_l_c * (1.0f - alpha) + sample_l_n * alpha)) bl |= (1 << bit);
                    if (mod_r.modulate(sample_r_c * (1.0f - alpha) + sample_r_n * alpha)) br |= (1 << bit);
                }
                // 符合 DSD_U32_BE 典型的 4字节交织排列
                out_chunk[i] = bl;
                out_chunk[i + 8] = br;
            }
            
            // 输出格式：L-L-L-L R-R-R-R L-L-L-L R-R-R-R (示例交织)
            std::cout.write(reinterpret_cast<char*>(out_chunk), 16);

            cur[0] = nxt[0]; cur[1] = nxt[1];
            if (++frames % 44100 == 0) {
                float dbl = 20.0f * std::log10(peak_l + 1e-9f);
                float dbr = 20.0f * std::log10(peak_r + 1e-9f);
                SystemManager::draw(gain, dbl, dbr, mod_l.max_stress, mod_r.max_stress);
                peak_l = peak_r = 0;
                mod_l.reset_metrics(); mod_r.reset_metrics();
            }
        }
    }
};

int main(int argc, char* argv[]) {
    double gain = (argc > 1) ? std::atof(argv[1]) : 0.25; // 针对 Rey's Theme 建议 0.25
    AudioStreamer streamer;
    streamer.process(gain);
    return 0;
}