#include <iostream>
#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>

struct alignas(64) SDM5 {
    float s[5] = {0,0,0,0,0};
    float q = 0.0f;
    const float LIMIT = 128.0f;
    float gain_factor;

    // 核心调制：保持你“挑食”后的原始数学逻辑
    inline int modulate(float input) {
        const float x = input * gain_factor;
        s[0] += (x - q);
        s[1] += (s[0] - q * 0.5f);
        s[2] += (s[1] - q * 0.25f);
        s[3] += (s[2] - q * 0.125f);
        s[4] += (s[3] - q * 0.0625f);

        if (s[0] >  LIMIT) s[0] =  LIMIT; else if (s[0] < -LIMIT) s[0] = -LIMIT;
        if (s[1] >  LIMIT) s[1] =  LIMIT; else if (s[1] < -LIMIT) s[1] = -LIMIT;
        if (s[2] >  LIMIT) s[2] =  LIMIT; else if (s[2] < -LIMIT) s[2] = -LIMIT;
        if (s[3] >  LIMIT) s[3] =  LIMIT; else if (s[3] < -LIMIT) s[3] = -LIMIT;
        if (s[4] >  LIMIT) s[4] =  LIMIT; else if (s[4] < -LIMIT) s[4] = -LIMIT;

        const int bit = (s[4] >= 0.0f);
        q = bit ? 1.0f : -1.0f;
        return bit;
    }
};

int main(int argc, char* argv[]) {
    const float target_gain = (argc > 1) ? static_cast<float>(std::atof(argv[1])) : 0.5f;
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(NULL);

    SDM5 mod_l, mod_r;
    mod_l.gain_factor = mod_r.gain_factor = target_gain;
    float cur[2], nxt[2];
    const float inv_64 = 1.0f / 64.0f;

    if (!std::cin.read(reinterpret_cast<char*>(cur), 8)) return 0;
    uint32_t out_block[4]; 

    while (std::cin.read(reinterpret_cast<char*>(nxt), 8)) {
        const float diff_l = nxt[0] - cur[0];
        const float diff_r = nxt[1] - cur[1];

        // 64倍插值：前32个点合成一组L/R，后32个点合成一组L/R
        for (int half = 0; half < 2; ++half) {
            uint32_t ul = 0, ur = 0;
            const int offset = half * 32;
            for (int b = 0; b < 32; ++b) {
                const float alpha = static_cast<float>(offset + b) * inv_64;
                if (mod_l.modulate(cur[0] + diff_l * alpha)) ul |= (1U << (31 - b));
                if (mod_r.modulate(cur[1] + diff_r * alpha)) ur |= (1U << (31 - b));
            }
            out_block[half * 2]     = __builtin_bswap32(ul); // 转大端
            out_block[half * 2 + 1] = __builtin_bswap32(ur); // 转大端
        }
        std::cout.write(reinterpret_cast<char*>(out_block), 16);
        cur[0] = nxt[0]; cur[1] = nxt[1];
    }
    return 0;
}