#ifndef AUTODBG_COMMON_DYNAPI_HPP
#define AUTODBG_COMMON_DYNAPI_HPP
/* =============================================================================
 * src/common/dynapi.hpp —— 新 API 动态加载层（Plan.md 4.8 / P2.5）
 *
 * 用途：x86 产物要加载到 Win98，导入表里**不能**出现 XP+/Vista+ 的 API。
 *   因此对「可选增强」型新 API 一律走本层：解析成功即启用，失败即降级。
 *
 * 铁律（Plan 4.8）：
 *   ① **x86 代码路径不得以新 API 为前提** —— 老系统必需的功能必须用老系统可用的
 *      等价实现，或该功能在老系统上整体关闭；
 *   ② 解析失败**不致命**：`get_proc` 返回 nullptr 是正常路径（老系统无此增强）；
 *   ③ 不用链接器延迟加载（导入表/失败处理不可控），也不用宏替换直接调用。
 *
 * 既有正确范例（P3 会统一收敛到这里）：`DebugActiveProcessStop`、`StackWalk`、`StackWalk64`。
 *
 * 实现细节：`GetModuleHandleA` 命中就直接取（不无谓加载 DLL），否则 `LoadLibraryA`；
 *   结果按 (模块, 符号) 在进程内**小容量线性表**里缓存（调试器是单线程，无需加锁）。
 *   `LoadLibraryA` / `GetProcAddress` / `GetModuleHandleA` 均为 Win95+ 可用。
 * ========================================================================== */
#include <stddef.h>

namespace autodbg::common
{
/* 解析符号；失败返回 nullptr（调用方必须处理降级路径） */
void *get_proc(const char *module_name, const char *symbol) noexcept;

/* 便捷：直接转成函数指针类型。用法：
 *   using IsWow64ProcessT = BOOL(WINAPI*)(HANDLE, PBOOL);
 *   static IsWow64ProcessT p = get_proc_as<IsWow64ProcessT>("kernel32.dll", "IsWow64Process");
 *   if (p && p(h, &wow)) { ... } */
template <class FnT>
inline FnT get_proc_as(const char *module_name, const char *symbol) noexcept
{
    return reinterpret_cast<FnT>(get_proc(module_name, symbol));
}

/* 仅供诊断/测试：当前缓存条目数 */
size_t dynapi_cache_count() noexcept;
}

#endif /* AUTODBG_COMMON_DYNAPI_HPP */
