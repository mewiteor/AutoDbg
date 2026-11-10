/* =============================================================================
 * src/common/allocator.cpp —— 全局 operator new/delete + alloc/release/resize
 *
 * 失败策略（与 Plan.md 4.2 的「不启用异常」一致）：
 *   分配失败时**打印一行诊断并 ExitProcess(3)**，而不是返回 nullptr。
 *   理由：无异常模式下 C++ 的 `new` 表达式不会替我们检查 nullptr（那会导致构造被写到空指针上），
 *         而当前代码库的既有风格也是「假定分配成功」（`alloc_` 宏直接 malloc 后不解引用检查）。
 *   注意：`operator new` 的声明必须与隐式声明一致（**不能**加 noexcept）——见下。
 * ========================================================================== */
#include "allocator.hpp"

#include <stdlib.h>  /* malloc / free / realloc（零 CRT：由 src/sys/msvcrt.def 导入） */
#include <windows.h> /* GetStdHandle / WriteFile / ExitProcess（诊断用，Win95+ 可用） */

namespace autodbg::common
{
[[noreturn]] static void alloc_failed(size_t size)
{
    (void)size;
    static const char msg[] = "[autodbg] fatal: out of memory\n";
    DWORD             written = 0;
    HANDLE            err     = GetStdHandle(STD_ERROR_HANDLE);
    if (err && err != INVALID_HANDLE_VALUE)
        WriteFile(err, msg, (DWORD)(sizeof msg - 1), &written, NULL);
    ExitProcess(3);
}

void *alloc(size_t size) noexcept
{
    if (!size) size = 1; /* malloc(0) 的行为依实现而定 → 统一按 1 字节 */
    void *p = malloc(size);
    if (!p) alloc_failed(size);
    return p;
}

void *resize(void *p, size_t new_size) noexcept
{
    if (!new_size)
    {
        release(p);
        return nullptr;
    }
    void *q = realloc(p, new_size);
    if (!q) alloc_failed(new_size);
    return q;
}

void release(void *p) noexcept
{
    if (p) free(p);
}
}

/* ---- 全局替换（不在此处重新声明，以免与隐式声明签名不一致） ----
 * sized dealloc（`operator delete(void*, size_t)`）在 C++14+ 默认启用，
 * 因此必须一并定义，否则某些场景会出现未解析符号。 */
void *operator new(size_t size)
{
    return autodbg::common::alloc(size);
}

void *operator new[](size_t size)
{
    return autodbg::common::alloc(size);
}

void operator delete(void *p) noexcept
{
    autodbg::common::release(p);
}

void operator delete[](void *p) noexcept
{
    autodbg::common::release(p);
}

void operator delete(void *p, size_t) noexcept
{
    autodbg::common::release(p);
}

void operator delete[](void *p, size_t) noexcept
{
    autodbg::common::release(p);
}
