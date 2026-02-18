#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <thread>
#include <atomic>
#include <vector>
#include <cstring>
#include <chrono>

#ifdef __linux__
  #include <sched.h>
  #include <pthread.h>
#endif

// 字节序转换（ALSA DSD_U32_BE 必须）
static inline uint32_t bswap32(uint32_t x) noexcept { return __builtin_bswap32(x); }

// ===================== 统一的 SDM 引擎 (NEON vs Scalar) =====================
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
  #include <arm_neon.h>
  struct alignas(64) SDM5_Engine {
    float32x2_t s0, s1, s2, s3, s4, q, gain, vLimit, vNegLimit;
    SDM5_Engine(float g) noexcept {
        s0 = s1 = s2 = s3 = s4 = vdup_n_f32(0.0f);
        q = vdup_n_f32(0.0f); gain = vdup_n_f32(g);
        vLimit = vdup_n_f32(128.0f); vNegLimit = vdup_n_f32(-128.0f);
    }
    inline uint32_t modulate2(float32x2_t input) noexcept {
        const float32x2_t x = vmul_f32(input, gain);
        s0 = vadd_f32(s0, vsub_f32(x, q));
        s1 = vadd_f32(s1, vsub_f32(s0, vmul_n_f32(q, 0.5f)));
        s2 = vadd_f32(s2, vsub_f32(s1, vmul_n_f32(q, 0.25f)));
        s3 = vadd_f32(s3, vsub_f32(s2, vmul_n_f32(q, 0.125f)));
        s4 = vadd_f32(s4, vsub_f32(s3, vmul_n_f32(q, 0.0625f)));
        s0 = vmin_f32(vmax_f32(s0, vNegLimit), vLimit);
        s1 = vmin_f32(vmax_f32(s1, vNegLimit), vLimit);
        s2 = vmin_f32(vmax_f32(s2, vNegLimit), vLimit);
        s3 = vmin_f32(vmax_f32(s3, vNegLimit), vLimit);
        s4 = vmin_f32(vmax_f32(s4, vNegLimit), vLimit);
        const uint32x2_t mask = vcge_f32(s4, vdup_n_f32(0.0f));
        q = vbsl_f32(mask, vdup_n_f32(1.0f), vdup_n_f32(-1.0f));
        return ((vget_lane_u32(mask, 0) >> 31) & 1u) | (((vget_lane_u32(mask, 1) >> 31) & 1u) << 1);
    }
  };
#else
  // 非 NEON 环境的 Fallback (用于编译通过)
  struct SDM5_Engine {
      float sL[5]={0}, sR[5]={0}, qL=0, qR=0, gain;
      SDM5_Engine(float g) : gain(g) {}
      inline uint32_t modulate2(struct {float l, r;} in) {
          auto step = [&](float i, float* s, float& q) {
              float x = i * gain;
              s[0]+=(x-q); s[1]+=(s[0]-q*0.5f); s[2]+=(s[1]-q*0.25f);
              s[3]+=(s[2]-q*0.125f); s[4]+=(s[3]-q*0.0625f);
              for(int j=0; j<5; ++j) { if(s[j]>128.f) s[j]=128.f; else if(s[j]<-128.f) s[j]=-128.f; }
              int b=(s[4]>=0); q=b?1.f:-1.f; return (uint32_t)b;
          };
          return step(in.l, sL, qL) | (step(in.r, sR, qR) << 1);
      }
  };
#endif

// ===================== 并行同步逻辑 =====================
constexpr int BATCH = 8192;   
constexpr int QUEUE_SIZE = 8; 

struct Payload {
    float in_data[BATCH][2];
    uint32_t out_data[BATCH][4];
    size_t n = 0;
    std::atomic<bool> ready{false}; 
};

Payload pool[QUEUE_SIZE];
std::atomic<size_t> producer_idx{0};
std::atomic<size_t> consumer_idx{0};
std::atomic<bool> stop_flag{false};

void worker_func(int core_id, float gain) {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#endif

    SDM5_Engine mod(gain);
    float curL = 0, curR = 0;

    while (!stop_flag) {
        size_t c_idx = consumer_idx.load(std::memory_order_acquire);
        Payload& p = pool[c_idx % QUEUE_SIZE];

        // 检查是否有新数据块进入队列且尚未处理
        if (p.n > 0 && !p.ready.load(std::memory_order_relaxed)) {
            for (size_t i = 0; i < p.n; ++i) {
                float nxtL = p.in_data[i][0], nxtR = p.in_data[i][1];
                float stepL = (nxtL - curL) * (1.0f/64.0f), stepR = (nxtR - curR) * (1.0f/64.0f);
                uint32_t L0=0, R0=0, L1=0, R1=0;

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
                float32x2_t v = {curL, curR}, v_step = {stepL, stepR};
                for(int b=0; b<32; ++b) { 
                    uint32_t bits = mod.modulate2(v);
                    L0 = (L0<<1)|(bits&1u); R0 = (R0<<1)|((bits>>1)&1u); v = vadd_f32(v, v_step);
                }
                for(int b=0; b<32; ++b) {
                    uint32_t bits = mod.modulate2(v);
                    L1 = (L1<<1)|(bits&1u); R1 = (R1<<1)|((bits>>1)&1u); v = vadd_f32(v, v_step);
                }
#else
                struct {float l, r;} v = {curL, curR};
                for(int b=0; b<32; ++b) {
                    uint32_t bits = mod.modulate2({v.l, v.r});
                    L0=(L0<<1)|(bits&1u); R0=(R0<<1)|((bits>>1)&1u); v.l+=stepL; v.r+=stepR;
                }
                for(int b=0; b<32; ++b) {
                    uint32_t bits = mod.modulate2({v.l, v.r});
                    L1=(L1<<1)|(bits&1u); R1=(R1<<1)|((bits>>1)&1u); v.l+=stepL; v.r+=stepR;
                }
#endif
                p.out_data[i][0] = bswap32(L0); p.out_data[i][1] = bswap32(R0);
                p.out_data[i][2] = bswap32(L1); p.out_data[i][3] = bswap32(R1);
                curL = nxtL; curR = nxtR;
            }
            p.ready.store(true, std::memory_order_release);
            consumer_idx.fetch_add(1, std::memory_order_release);
        } else {
            // 关键：避免 100% 占用的微睡眠
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
}

int main(int argc, char** argv) {
    float gain = (argc > 1) ? (float)std::atof(argv[1]) : 0.5f;
    std::setvbuf(stdin,  nullptr, _IOFBF, 2<<20);
    std::setvbuf(stdout, nullptr, _IOFBF, 2<<20);

    std::thread worker(worker_func, 1, gain); // 绑定 Core 1

    size_t write_idx = 0;
    while (true) {
        size_t p_idx = producer_idx.load(std::memory_order_acquire);
        Payload& p = pool[p_idx % QUEUE_SIZE];

        // 生产者：如果槽位空闲，读取数据
        if (!p.ready.load(std::memory_order_relaxed)) {
            p.n = std::fread(p.in_data, sizeof(float)*2, BATCH, stdin);
            if (p.n == 0) break; 
            producer_idx.fetch_add(1, std::memory_order_release);
        }

        // 消费者：检查是否有处理完的块并输出
        Payload& out_p = pool[write_idx % QUEUE_SIZE];
        if (out_p.ready.load(std::memory_order_acquire)) {
            if (std::fwrite(out_p.out_data, sizeof(uint32_t)*4, out_p.n, stdout) != out_p.n) break;
            out_p.n = 0; 
            out_p.ready.store(false, std::memory_order_release);
            write_idx++;
        } else {
            // 主线程也进行微睡眠
            std::this_thread::sleep_for(std::chrono::microseconds(150));
        }
    }

    stop_flag = true;
    worker.join();
    return 0;
}