/* =============================================================================
 * src/common/dynapi.cpp —— 动态加载层实现（见 dynapi.hpp 的说明）
 * ========================================================================== */
#include "dynapi.hpp"
#include <windows.h>

namespace autodbg::common
{
namespace
{
struct Entry
{
    const char *module; /* 字面量指针即可（调用点都传字面量） */
    const char *symbol;
    void       *fn;
};

constexpr size_t kCacheCapacity = 64;
Entry            g_cache[kCacheCapacity];
size_t           g_cache_count = 0;
}

void *get_proc(const char *module_name, const char *symbol) noexcept
{
    if (!module_name || !symbol) return nullptr;

    /* 1) 查缓存（线性扫描：条目极少，够用；调试器单线程，无需加锁） */
    for (size_t i = 0; i < g_cache_count; i++)
    {
        /* 用指针比较即可：调用点传入的都是字面量（与缓存里存的是同一地址） */
        if (g_cache[i].module == module_name && g_cache[i].symbol == symbol) return g_cache[i].fn;
    }

    /* 2) 解析：优先已加载模块（避免无谓加载 DLL），否则 LoadLibrary */
    HMODULE mod = GetModuleHandleA(module_name);
    if (!mod) mod = LoadLibraryA(module_name);
    void *fn = mod ? reinterpret_cast<void *>(GetProcAddress(mod, symbol)) : nullptr;
    /* 注：解析成功即让模块常驻（不 FreeLibrary）—— 这些是系统 DLL，本就常驻 */

    /* 3) 记缓存（失败结果也缓存：避免反复尝试；容量满则不再记，不影响正确性） */
    if (g_cache_count < kCacheCapacity)
    {
        g_cache[g_cache_count].module = module_name;
        g_cache[g_cache_count].symbol = symbol;
        g_cache[g_cache_count].fn     = fn;
        g_cache_count++;
    }
    return fn;
}

size_t dynapi_cache_count() noexcept
{
    return g_cache_count;
}
}
