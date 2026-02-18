// #include <iostream>
// #include <vector>
// #include <cstdint>
// #include <cmath>
// #include <algorithm>

// // 保持 64 字节对齐，防止多线程下的缓存行失效（虽然是单线程，但这是好习惯）
// struct alignas(64) SDM5 {
//     double s[5] = {0,0,0,0,0};
//     double q = 0;
//     const double LIMIT = 100.0;
//     double gain_factor = 0.5;

//     // 极致优化：去掉所有监控计数、Peak 记录和 Reset 逻辑
//     inline int modulate(double input) {
//         const double x = input * gain_factor;
        
//         // --- 核心音频链路逻辑：绝对不动 ---
//         s[0] += (x - q);
//         s[1] += (s[0] - q * 0.5);
//         s[2] += (s[1] - q * 0.25);
//         s[3] += (s[2] - q * 0.125);
//         s[4] += (s[3] - q * 0.0625);

//         // 仅保留必要的 Hard Clip 保护，防止数值飞出 double 范围
//         for (int i = 0; i < 5; ++i) {
//             if (s[i] > LIMIT) s[i] = LIMIT;
//             else if (s[i] < -LIMIT) s[i] = -LIMIT;
//         }

//         int bit = (s[4] >= 0) ? 1 : 0;
//         q = bit ? 1.0 : -1.0;
//         return bit;
//     }
// };

// int main(int argc, char* argv[]) {
//     double target_gain = (argc > 1) ? std::atof(argv[1]) : 0.5;

//     // 提升 IO 效率
//     std::ios_base::sync_with_stdio(false);
//     std::cin.tie(NULL);

//     SDM5 mod_l, mod_r;
//     mod_l.gain_factor = mod_r.gain_factor = target_gain;

//     float cur[2], nxt[2];
//     uint8_t out_l[8], out_r[8];

//     // 预读取第一帧
//     if (!std::cin.read(reinterpret_cast<char*>(cur), 8)) return 0;

//     // 彻底移除 stderr 的实时 UI 渲染，只跑音频逻辑
//     while (std::cin.read(reinterpret_cast<char*>(nxt), 8)) {
        
//         // DSD512 64倍插值处理
//         for (int i = 0; i < 8; ++i) {
//             uint8_t bl = 0, br = 0;
//             for (int bit = 7; bit >= 0; --bit) {
//                 // 线性插值
//                 float alpha = static_cast<float>(i * 8 + (7 - bit)) / 64.0f;
//                 double pl = (double)cur[0] * (1.0f - alpha) + (double)nxt[0] * alpha;
//                 double pr = (double)cur[1] * (1.0f - alpha) + (double)nxt[1] * alpha;
                
//                 if (mod_l.modulate(pl)) bl |= (1 << bit);
//                 if (mod_r.modulate(pr)) br |= (1 << bit);
//             }
//             out_l[i] = bl; out_r[i] = br;
//         }

//         // 紧凑的输出逻辑：对齐 DSD512 的双声道交织格式
//         std::cout.write(reinterpret_cast<char*>(&out_l[0]), 4);
//         std::cout.write(reinterpret_cast<char*>(&out_r[0]), 4);
//         std::cout.write(reinterpret_cast<char*>(&out_l[4]), 4);
//         std::cout.write(reinterpret_cast<char*>(&out_r[4]), 4);

//         cur[0] = nxt[0]; cur[1] = nxt[1];
//     }
//     return 0;
// }




#include <iostream>
#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <iomanip>

struct alignas(64) SDM5 {
    double s[5] = {0,0,0,0,0};
    double q = 0;
    const double LIMIT = 100.0;
    double gain_factor = 0.5;

    // 监控变量（仅在核心循环中做最简操作）
    uint64_t clip_total = 0;
    uint64_t sample_total = 0;
    double pcm_peak = 0;
    double s4_max = 0;

    inline int modulate(double input) {
        const double x = input * gain_factor;
        sample_total++;

        // 核心算法
        s[0] += (x - q);
        s[1] += (s[0] - q * 0.5);
        s[2] += (s[1] - q * 0.25);
        s[3] += (s[2] - q * 0.125);
        s[4] += (s[3] - q * 0.0625);

        // 记录 PCM 峰值
        double abs_x = (x < 0) ? -x : x;
        if (abs_x > pcm_peak) pcm_peak = abs_x;

        // Clip 检查与记录
        double abs_s4 = (s[4] < 0) ? -s[4] : s[4];
        if (abs_s4 > s4_max) s4_max = abs_s4;
        
        for (int i = 0; i < 5; ++i) {
            if (s[i] > LIMIT) { s[i] = LIMIT; clip_total++; }
            else if (s[i] < -LIMIT) { s[i] = -LIMIT; clip_total++; }
        }

        int bit = (s[4] >= 0) ? 1 : 0;
        q = bit ? 1.0 : -1.0;
        return bit;
    }

    void reset_metrics() {
        clip_total = 0;
        sample_total = 0;
        pcm_peak = 0;
        s4_max = 0;
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

    // 预清屏
    std::cerr << "\033[2J\033[H\033[?25l";

    while (std::cin.read(reinterpret_cast<char*>(nxt), 8)) {
        for (int i = 0; i < 8; ++i) {
            uint8_t bl = 0, br = 0;
            for (int bit = 7; bit >= 0; --bit) {
                float alpha = static_cast<float>(i * 8 + (7 - bit)) / 64.0f;
                if (mod_l.modulate(cur[0]*(1.0-alpha) + nxt[0]*alpha)) bl |= (1 << bit);
                if (mod_r.modulate(br = mod_r.modulate(cur[1]*(1.0-alpha) + nxt[1]*alpha))) br |= (1 << bit);
            }
            out_l[i] = bl; out_r[i] = br;
        }

        std::cout.write(reinterpret_cast<char*>(&out_l[0]), 4);
        std::cout.write(reinterpret_cast<char*>(&out_r[0]), 4);
        std::cout.write(reinterpret_cast<char*>(&out_l[4]), 4);
        std::cout.write(reinterpret_cast<char*>(&out_r[4]), 4);

        cur[0] = nxt[0]; cur[1] = nxt[1];
        frame_count++;

        // 每秒更新10次 (约 38400 帧更新一次)
        if (frame_count % 38400 == 0) {
            auto print_stat = [&](const char* label, SDM5& m) {
                double db = 20.0 * std::log10(m.pcm_peak + 1e-9);
                double clip_pct = (m.sample_total > 0) ? (double)m.clip_total / (m.sample_total * 5) * 100.0 : 0;
                std::cerr << label << " | PCM: " << std::fixed << std::setprecision(1) << std::setw(5) << db << " dB"
                          << " | S4: " << std::setw(5) << (int)m.s4_max 
                          << " | CLIP: " << std::setw(6) << m.clip_total 
                          << " (" << clip_pct << "%)\n";
                m.reset_metrics();
            };

            std::cerr << "\033[H"; // 光标回到左上角
            std::cerr << "--- DSD512 MONITOR (10Hz) ---\n";
            print_stat("LEFT ", mod_l);
            print_stat("RIGHT", mod_r);
            std::cerr << std::flush;
        }
    }
    return 0;
}