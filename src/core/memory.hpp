#ifndef AUTODBG_CORE_MEMORY_HPP
#define AUTODBG_CORE_MEMORY_HPP
/* =============================================================================
 * src/core/memory.hpp —— 目标进程内存读写（P3.B 模块）
 *
 * 三个原语失败时返回 false（调用方通常再取 GetLastError / 自行报错）。
 * 依赖：会话上下文（P6.4：本头里的函数都是 `DebuggerSession` 成员，用 `proc()` 取句柄）。
 * ========================================================================== */
#include "core.hpp"   /* P6.4：引擎函数已是 DebuggerSession 成员（下面的过渡转发也要它） */
#include "types.hpp"

/* 先读旧值到 old_data（可为 NULL），再写 new_data（可为 NULL）；is_up_intrin 决定是否刷指令缓存 */

/* 从目标内存读一条 NUL 结尾的字符串（每次 16 字节分块，可跨页），把累积缓冲返回给 *pp。
 * ❗1:1 保留旧 `bindings.cpp` 的语义：
 *   · `*pp` 为 `resize` 累积出来的缓冲（调用方用 `free_`/`common::release` 释放）；
 *   · `*nn` 是**不含结尾 NUL 的长度**，且缓冲**不额外分配终止符**（旧实现如此）；
 *   · 失败（读内存失败）返回 false，此时 `*nn = 0`（`*pp` 可能已被分配，调用方仍应释放）。
 * 唯一替换：`realloc` → `common::resize`（同一 msvcrt `realloc`；OOM 时 fail-fast，
 *   与 `alloc_`/`free_` 桥接策略一致）。 */

/* ============================ 内存搜索 / 区域遍历（P4.6 从绑定层下沉） ============================
 * 语义与旧 `bindings.cpp` 的 `lua_find_memory*` / `lua_get_memory_region` **1:1**：
 *   · 搜索用 Boyer-Moore-Horspool 的 shift 表，只扫 `PAGE_READWRITE` / `PAGE_EXECUTE_READWRITE`，
 *     且跳过比 pattern 小的区域；
 *   · 错误路径的状态/释放动作与文案都保持原样（见 `.cpp` 里的注释）。
 * 绑定层只负责：建 Lua 表 / 把 `errbuf` 里的文案 `lua_error`。 */



#endif /* AUTODBG_CORE_MEMORY_HPP */
