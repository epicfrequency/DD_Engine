#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <thread>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <cstring>

// 核心绑定需要的头文件
#ifdef __linux__
  #include <sched.h>
  #include <pthread.h>
#endif

// 字节序转换
static inline uint32_t bswap32(uint32_t x) noexcept { return __builtin_bswap32(x); }

// NEON 检测与定义
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
  #include <arm_neon.h>
  struct alignas(64) SDM5_neon2 {
    float32x2_t s0, s1, s2, s3, s4, q, gain, vLimit, vNegLimit;
    SDM5_neon2(float g) noexcept {
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
  // 回退到标量模式以防在非 NEON 环境测试
  struct SDM5_scalar {
      float s[5]={0}, q=0, gain;
      SDM5_scalar(float g) : gain(g) {}
      inline int modulate(float in) {
          float x = in * gain;
          s[0]+=(x-q); s[1]+=(s[0]-q*0.5f); s[2]+=(s[1]-q*0.25f);
          s[3]+=(s[2]-q*0.125f); s[4]+=(s[3]-q*0.0625f);
          for(int j=0; j<5; ++j) { if(s[j]>128.f) s[j]=128.f; else if(s[j]<-128.f) s[j]=-128.f; }
          int b=(s[4]>=0); q=b?1.f:-1.f; return b;
      }
  };
#endif

// 并行参数
constexpr int BATCH = 1024;
struct Payload {
    float in_data[BATCH][2];
    uint32_t out_data[BATCH][4];
    size_t n;
};

// 线程间同步
std::queue<Payload*> work_queue;
std::queue<Payload*> free_queue;
std::mutex mtx_work, mtx_free, mtx_stdout;
std::condition_variable cv_work;
std::atomic<bool> done{false};

void worker_func(int core_id, float gain) {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#endif

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    SDM5_neon2 mod(gain);
#else
    SDM5_scalar modL(gain), modR(gain);
#endif

    float curL = 0, curR = 0;
    while (true) {
        Payload* p = nullptr;
        {
            std::unique_lock<std::mutex> lock(mtx_work);
            cv_work.wait(lock, [] { return !work_queue.empty() || done; });
            if (work_queue.empty() && done) break;
            p = work_queue.front();
            work_queue.pop();
        }

        for (size_t i = 0; i < p->n; ++i) {
            float nxtL = p->in_data[i][0], nxtR = p->in_data[i][1];
            float stepL = (nxtL - curL) * (1.0f/64.0f), stepR = (nxtR - curR) * (1.0f/64.0f);
            uint32_t L0=0, R0=0, L1=0, R1=0;

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
            float32x2_t v = {curL, curR}, step = {stepL, stepR};
            for(int b=0; b<32; ++b) { 
                uint32_t bits = mod.modulate2(v);
                L0 = (L0<<1)|(bits&1u); R0 = (R0<<1)|((bits>>1)&1u); v = vadd_f32(v, step);
            }
            for(int b=0; b<32; ++b) {
                uint32_t bits = mod.modulate2(v);
                L1 = (L1<<1)|(bits&1u); R1 = (R1<<1)|((bits>>1)&1u); v = vadd_f32(v, step);
            }
#else
            float vL = curL, vR = curR;
            for(int b=0; b<32; ++b) {
                L0 = (L0<<1)|(uint32_t)modL.modulate(vL); R0 = (R0<<1)|(uint32_t)modR.modulate(vR);
                vL += stepL; vR += stepR;
            }
            for(int b=0; b<32; ++b) {
                L1 = (L1<<1)|(uint32_t)modL.modulate(vL); R1 = (R1<<1)|(uint32_t)modR.modulate(vR);
                vL += stepL; vR += stepR;
            }
#endif
            p->out_data[i][0] = bswap32(L0); p->out_data[i][1] = bswap32(R0);
            p->out_data[i][2] = bswap32(L1); p->out_data[i][3] = bswap32(R1);
            curL = nxtL; curR = nxtR;
        }

        // 输出并回收缓冲区
        {
            std::lock_guard<std::mutex> lock(mtx_stdout);
            std::fwrite(p->out_data, sizeof(uint32_t)*4, p->n, stdout);
        }
        {
            std::lock_guard<std::mutex> lock(mtx_free);
            free_queue.push(p);
        }
    }
}

int main(int argc, char** argv) {
    float gain = (argc > 1) ? (float)std::atof(argv[1]) : 0.5f;
    std::setvbuf(stdin,  nullptr, _IOFBF, 1<<20);
    std::setvbuf(stdout, nullptr, _IOFBF, 1<<20);

    // 预分配缓冲区
    for(int i=0; i<4; ++i) free_queue.push(new Payload());

    // 启动两个计算线程，绑定到核心 1 和 2
    std::thread t1(worker_func, 1, gain);
    std::thread t2(worker_func, 2, gain);

    while (true) {
        Payload* p = nullptr;
        {
            std::lock_guard<std::mutex> lock(mtx_free);
            if (!free_queue.empty()) { p = free_queue.front(); free_queue.pop(); }
        }
        
        if (!p) { std::this_thread::yield(); continue; }

        p->n = std::fread(p->in_data, sizeof(float)*2, BATCH, stdin);
        if (p->n == 0) { done = true; break; }

        {
            std::lock_guard<std::mutex> lock(mtx_work);
            work_queue.push(p);
            cv_work.notify_one();
        }
    }

    cv_work.notify_all();
    t1.join(); t2.join();
    return 0;
}