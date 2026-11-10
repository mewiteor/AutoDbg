/* =============================================================================
 * src/common/rand.cpp —— 随机字节实现（server token 熵源）
 *
 * 来源：新写（plan_server.md S0）。
 * 与旧实现的差异：无（新文件）。
 * 注意：`SystemFunction036` 在 w95/w98 的 advapi32 里没有（实测 §1.4）→ 必须走 dynapi；
 *   静态导入会让 legacy 模块在 9x 上映射失败（导入解析是硬失败）。
 * ========================================================================== */
#include "rand.hpp"

#include <windows.h>

#include "dynapi.hpp"

namespace autodbg::common
{
namespace
{
/* 静态计数器：同一次进程里连续取也不会拿到相同序列 */
unsigned g_counter = 0;

/* 降级混合：时间 + 性能计数器 + PID/TID + 静态计数器，再走 32 位 xorshift */
struct fallback_state
{
    unsigned x;
    unsigned y;
    unsigned z;
    unsigned w;
};

fallback_state g_fb = {0x9E3779B9u, 0x85EBCA6Bu, 0xC2B2AE35u, 0x27D4EB2Fu};

unsigned next_fallback()
{
    /* xorshift128（够用即可；**不是**密码学安全，见头文件说明） */
    unsigned t = g_fb.x ^ (g_fb.x << 11);
    g_fb.x     = g_fb.y;
    g_fb.y     = g_fb.z;
    g_fb.z     = g_fb.w;
    g_fb.w      = g_fb.w ^ (g_fb.w >> 19) ^ t ^ (t >> 8);
    return g_fb.w;
}

void seed_fallback()
{
    LARGE_INTEGER qpc;
    qpc.QuadPart = 0;
    QueryPerformanceCounter(&qpc);

    unsigned mix = 0;
    mix ^= (unsigned)GetTickCount();
    mix ^= (unsigned)qpc.LowPart;
    mix ^= (unsigned)qpc.HighPart;
    mix ^= (unsigned)GetCurrentProcessId() << 7;
    mix ^= (unsigned)GetCurrentThreadId() << 13;
    mix ^= (unsigned)++g_counter * 2654435761u;

    g_fb.x ^= mix;
    g_fb.y ^= mix * 1103515245u + 12345u;
    g_fb.z ^= (mix >> 3) ^ 0x5BD1E995u;
    g_fb.w ^= (mix << 5) ^ 0x165667B1u;
    for (int i = 0; i < 8; i++) /* 打散一次，避免种子相关性 */
        next_fallback();
}

typedef BOOLEAN(__stdcall *rtl_gen_random_fn)(PVOID, ULONG);

rtl_gen_random_fn resolve_gen_random()
{
    /* dynapi 自带缓存：首次解析后命中线性表 */
    return get_proc_as<rtl_gen_random_fn>("advapi32.dll", "SystemFunction036");
}
} /* namespace */

void rand_bytes(void *dst, size_t n) noexcept
{
    if (dst == nullptr || n == 0)
        return;

    unsigned char *p = (unsigned char *)dst;

    rtl_gen_random_fn fn = resolve_gen_random();
    while (fn != nullptr && n > 0)
    {
        ULONG chunk = (n > 0x1000u) ? 0x1000u : (ULONG)n;
        if (!fn(p, chunk))
            break; /* 系统调用失败 → 剩下的走降级 */
        p += chunk;
        n -= chunk;
    }
    if (n == 0)
        return;

    if (g_fb.x == 0x9E3779B9u && g_fb.w == 0x27D4EB2Fu)
        seed_fallback();

    size_t i = 0;
    while (i < n)
    {
        unsigned v = next_fallback();
        for (int k = 0; k < 4 && i < n; k++, i++)
            p[i] = (unsigned char)((v >> (k * 8)) & 0xFFu);
    }
}

bool rand_hex(char *dst, size_t cap, size_t n_bytes) noexcept
{
    static const char k_hex[] = "0123456789abcdef";

    if (dst == nullptr)
        return false;
    if (cap < n_bytes * 2 + 1)
    {
        if (cap > 0)
            dst[0] = '\0';
        return false;
    }

    unsigned char buf[64];
    if (n_bytes > sizeof buf)
        n_bytes = sizeof buf; /* 上限 64 字节（128 hex 字符）足够 token 用 */
    rand_bytes(buf, n_bytes);

    for (size_t i = 0; i < n_bytes; i++)
    {
        dst[i * 2]     = k_hex[(buf[i] >> 4) & 0xF];
        dst[i * 2 + 1] = k_hex[buf[i] & 0xF];
    }
    dst[n_bytes * 2] = '\0';
    return true;
}
} /* namespace autodbg::common */
