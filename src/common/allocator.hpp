#ifndef AUTODBG_COMMON_ALLOCATOR_HPP
#define AUTODBG_COMMON_ALLOCATOR_HPP
/* =============================================================================
 * src/common/allocator.hpp —— 零 CRT 分配入口（Plan.md P2.1）
 *
 * 目的：`/nodefaultlib` 下没有 CRT 的 `operator new`，而 C++ 化后必然要用 `new`。
 * 做法：全局 `operator new/delete/new[]/delete[]` 全部委托到本文件的 `alloc/release`，
 *       后者薄封装 msvcrt 的 `malloc/free/realloc`（符号见 `src/sys/msvcrt.def`，
 *       且实测在 32 位 `msvcrt_98.txt` 中 → Win98 起可用）。
 *
 * 命名说明：不用 `alloc_`/`free_`，因为 `src/debugger/lib.h` 目前把它们定义成**宏**
 *   （`#define alloc_ malloc`）；宏是 token 级替换，`autodbg::common::alloc` 不受影响。
 *   P3 删掉 lib.h 后可再议（当前命名更短、且不会与宏互踩）。
 * ========================================================================== */
#include <stddef.h>

namespace autodbg::common
{
/* 分配 size 字节（失败 = fail-fast，见 allocator.cpp；不会返回 nullptr） */
void *alloc(size_t size) noexcept;

/* 重新分配；new_size == 0 等价于释放并返回 nullptr */
void *resize(void *p, size_t new_size) noexcept;

/* 释放（p == nullptr 时什么也不做） */
void  release(void *p) noexcept;

/* 便捷：分配 count 个 T（不构造，仅分配；调用方自行初始化） */
template <class T>
T *alloc_array(size_t count) noexcept
{
    return static_cast<T *>(alloc(sizeof(T) * count));
}
}

#endif /* AUTODBG_COMMON_ALLOCATOR_HPP */
