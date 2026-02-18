// sdm5_dsd512_u32be_auto.cpp
// Linux-only, optimized for Pi 5 but builds on x86 too.
// Auto-selects NEON (ARM) vs scalar (others). Outputs ALSA native DSD_U32_BE.
//
// In : PCM f32le, interleaved stereo (L,R), 384000 Hz
// Out: DSD512 packed as 32-bit big-endian words, interleaved stereo
//      For each PCM step (cur->nxt) we output 4 words: L32 R32 L32 R32 (16 bytes)
//
// Build (Pi 5 / ARM64):
//   g++ -O3 -mcpu=cortex-a76 -ffast-math -funroll-loops -flto -fno-exceptions -fno-rtti -std=c++17 sdm5_dsd512_u32be_auto.cpp -o sdm5
//
// Build (x86_64):
//   g++ -O3 -march=native -ffast-math -funroll-loops -flto -fno-exceptions -fno-rtti -std=c++17 sdm5_dsd512_u32be_auto.cpp -o sdm5
//
// Run:
//   ffmpeg -i input.wav -f f32le -ac 2 -ar 384000 - \
//   | ./sdm5 0.5 \
//   | aplay -D hw:0,0 -c 2 -f DSD_U32_BE -r 24576000 --buffer-time=200000 --period-time=50000

#include <cstdint>
#include <cstdlib>
#include <cstdio>

static inline uint32_t bswap32(uint32_t x) noexcept { return __builtin_bswap32(x); }

#if defined(__aarch64__) || defined(__arm__)
  #define SDM_ARCH_ARM 1
#else
  #define SDM_ARCH_ARM 0
#endif

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
  #define SDM_HAS_NEON 1
#else
  #define SDM_HAS_NEON 0
#endif

#if SDM_ARCH_ARM && SDM_HAS_NEON
  #include <arm_neon.h>
#endif

// ===================== Scalar SDM (per-channel) =====================

struct alignas(64) SDM5_scalar {
    float s[5] = {0,0,0,0,0};
    float q = 0.0f;
    float gain_factor = 0.5f;
    static constexpr float LIMIT = 128.0f;

    inline int modulate(float input) noexcept {
        const float x = input * gain_factor;

        s[0] += (x    - q);
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

// ===================== NEON SDM (2-lane: L/R together) =====================
#if SDM_ARCH_ARM && SDM_HAS_NEON

struct alignas(64) SDM5_neon2 {
    float32x2_t s0, s1, s2, s3, s4;
    float32x2_t q;
    float32x2_t gain;
    float32x2_t vLimit, vNegLimit;

    explicit SDM5_neon2(float g) noexcept {
        s0 = s1 = s2 = s3 = s4 = vdup_n_f32(0.0f);
        q  = vdup_n_f32(0.0f);
        gain = vdup_n_f32(g);
        constexpr float LIMIT = 128.0f;
        vLimit    = vdup_n_f32(LIMIT);
        vNegLimit = vdup_n_f32(-LIMIT);
    }

    // returns bits packed in low2 bits: bit0=L, bit1=R
    inline uint32_t modulate2(float32x2_t input) noexcept {
        const float32x2_t x = vmul_f32(input, gain);

        s0 = vadd_f32(s0, vsub_f32(x, q));
        s1 = vadd_f32(s1, vsub_f32(s0, vmul_n_f32(q, 0.5f)));
        s2 = vadd_f32(s2, vsub_f32(s1, vmul_n_f32(q, 0.25f)));
        s3 = vadd_f32(s3, vsub_f32(s2, vmul_n_f32(q, 0.125f)));
        s4 = vadd_f32(s4, vsub_f32(s3, vmul_n_f32(q, 0.0625f)));

        // clamp without branches: s = min(max(s, -L), +L)
        s0 = vmin_f32(vmax_f32(s0, vNegLimit), vLimit);
        s1 = vmin_f32(vmax_f32(s1, vNegLimit), vLimit);
        s2 = vmin_f32(vmax_f32(s2, vNegLimit), vLimit);
        s3 = vmin_f32(vmax_f32(s3, vNegLimit), vLimit);
        s4 = vmin_f32(vmax_f32(s4, vNegLimit), vLimit);

        // bit = (s4 >= 0)
        const uint32x2_t mask = vcge_f32(s4, vdup_n_f32(0.0f));

        // q = bit ? +1 : -1
        const float32x2_t plus1  = vdup_n_f32(1.0f);
        const float32x2_t minus1 = vdup_n_f32(-1.0f);
        q = vbsl_f32(mask, plus1, minus1);

        // Extract 2 bits (lane0=L, lane1=R).
        // mask lanes are 0xFFFFFFFF for true else 0. Take top bit.
        const uint32_t m0 = (vget_lane_u32(mask, 0) >> 31) & 1u;
        const uint32_t m1 = (vget_lane_u32(mask, 1) >> 31) & 1u;
        return (m0) | (m1 << 1);
    }
};

#endif // NEON

// ===================== Main =====================

int main(int argc, char** argv) {
    const float gain = (argc > 1) ? static_cast<float>(std::atof(argv[1])) : 0.5f;

#if SDM_ARCH_ARM && SDM_HAS_NEON
    std::fprintf(stderr, "super Neon!\n");
#else
    std::fprintf(stderr, "scalar mode\n");
#endif

    // Big stdio buffers -> fewer syscalls in pipes
    std::setvbuf(stdin,  nullptr, _IOFBF, 1 << 20);
    std::setvbuf(stdout, nullptr, _IOFBF, 1 << 20);

    constexpr int BATCH = 512;  // tune 256/512/1024 for your pipeline
    float    in[BATCH][2];
    uint32_t out[BATCH][4];

    float curL = 0.0f, curR = 0.0f;
    bool have_cur = false;

#if SDM_ARCH_ARM && SDM_HAS_NEON
    SDM5_neon2 mod(gain);
#else
    SDM5_scalar modL, modR;
    modL.gain_factor = modR.gain_factor = gain;
#endif

    while (true) {
        const size_t n = std::fread(in, sizeof(float) * 2, BATCH, stdin);
        if (n == 0) break;

        size_t out_n = 0;

        for (size_t i = 0; i < n; ++i) {
            const float nxtL = in[i][0];
            const float nxtR = in[i][1];

            if (!have_cur) {
                curL = nxtL; curR = nxtR;
                have_cur = true;
                continue;
            }

            // 64x interpolation via stepping (no mul per bit)
            const float stepL = (nxtL - curL) * (1.0f / 64.0f);
            const float stepR = (nxtR - curR) * (1.0f / 64.0f);

            uint32_t L0 = 0, R0 = 0, L1 = 0, R1 = 0;

#if SDM_ARCH_ARM && SDM_HAS_NEON
            float32x2_t v    = { curL,  curR  };
            const float32x2_t step = { stepL, stepR };

            // Pack MSB-first: u = (u<<1) | bit
            for (int b = 0; b < 32; ++b) {
                const uint32_t bits = mod.modulate2(v); // bit0=L bit1=R
                L0 = (L0 << 1) | (bits & 1u);
                R0 = (R0 << 1) | ((bits >> 1) & 1u);
                v = vadd_f32(v, step);
            }
            for (int b = 0; b < 32; ++b) {
                const uint32_t bits = mod.modulate2(v);
                L1 = (L1 << 1) | (bits & 1u);
                R1 = (R1 << 1) | ((bits >> 1) & 1u);
                v = vadd_f32(v, step);
            }
#else
            float vL = curL, vR = curR;

            for (int b = 0; b < 32; ++b) {
                L0 = (L0 << 1) | static_cast<uint32_t>(modL.modulate(vL));
                R0 = (R0 << 1) | static_cast<uint32_t>(modR.modulate(vR));
                vL += stepL; vR += stepR;
            }
            for (int b = 0; b < 32; ++b) {
                L1 = (L1 << 1) | static_cast<uint32_t>(modL.modulate(vL));
                R1 = (R1 << 1) | static_cast<uint32_t>(modR.modulate(vR));
                vL += stepL; vR += stepR;
            }
#endif

            // ALSA DSD_U32_BE expects big-endian words
            out[out_n][0] = bswap32(L0);
            out[out_n][1] = bswap32(R0);
            out[out_n][2] = bswap32(L1);
            out[out_n][3] = bswap32(R1);
            ++out_n;

            curL = nxtL; curR = nxtR;
        }

        if (out_n) {
            if (std::fwrite(out, sizeof(uint32_t) * 4, out_n, stdout) != out_n) return 1;
        }
        if (n < static_cast<size_t>(BATCH)) break; // EOF
    }

    return 0;
}