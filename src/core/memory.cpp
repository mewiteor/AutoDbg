/* =============================================================================
 * src/core/memory.cpp —— 内存读写（P3.B 模块拆分，逐字从 bindings.cpp 迁入）
 *
 * 对应 TODO 阶段 3 的 memory.cpp；P4.6 又把 **FindMemoryContext / GetMemoryRegion 的搜索与遍历**
 * 从绑定层下沉到这里（旧实现只差“把结果推回 Lua 栈”那一步）。
 * 依赖：会话状态（`proc()`）+ util 的错误/日志。
 *
 * T7 切片 C1b 追加 `read_some`（plan_t7.md §2.2 第 12 条）：逐页读、在第一个读不了的页处停下，
 *   把「真实读到的字节数」交给调用方。`read()` 变成它的全有或全无包装（签名与 29 个契约名的
 *   可观察行为都不变），`write` / `update_memory` / `loadstr` 一个字未改。
 *
 * T7 切片 C2b（plan_t7.md §10 第 3 条）追加 **pid 重载**：`read_some` / `read` / `write` /
 *   `update_memory` / `loadstr` 的首参数都可以显式给进程 pid，句柄由 `handle_for_pid(pid)` 取
 *   （根进程 = `proc().hProcess`，其它进程 = 缓存的 `OpenProcess` 句柄）。旧签名全部**保留**并转发
 *   给 `proc().dwProcessId` ⇒ 29 个契约名与既有调用点零改动。`pid == 0` 等价于根进程。
 *   内存搜索（`find_memory_*`）与区域遍历（`get_memory_region_*`）不在本切片范围内，仍只认根进程。
 * ========================================================================== */
#include "memory.hpp"
#include "core.hpp" /* P6.4：引擎函数是会话成员（本 TU 多数定义即成员，直接用自身状态） */ /* 模块 11：proc() 等状态现在通过 current_session() 取（宏别名） */
#include "types.hpp"
#include "util.hpp"

#include <psapi.h>
#include <assert.h>
#include <intrin.h>
#include <strsafe.h>
#include <string.h>

using autodbg::core::DebuggerSession; /* P6.4：引擎函数已是它的成员 */
/* P6.4：内存搜索/区域遍历的纯数据已收进 types.hpp（core 命名空间） */
using autodbg::core::FindMemoryContext;
using autodbg::core::FindMemoryKind;
using autodbg::core::FindMemoryMatch;
using autodbg::core::MemRegionInfo;
using autodbg::core::MemRegionIter;

/* 一块（不超过一页）的读：旧 `read()` 的三步原样保留 ——
 * `VirtualProtectEx(PAGE_EXECUTE_READ)` → `ReadProcessMemory`（长度必须精确相等）→ 还原保护。
 * ❗本函数**不刷错误日志**：逐页路径里「碰到不可读的页」是**正常路径**（partial read 的全部意义），
 *   在那里报错会把一次正常的短读变成满屏噪音。诊断留给 `read()`（它失败时打一行，见下）。
 * ❗`ReadProcessMemory` 失败时按 0 字节算，**即使它报了部分拷贝** —— 旧 `read()` 对任何
 * `ReadProcessMemory` 失败都整体返回 false，这一条不能因为有了 `read_some` 就松动。
 * ❗C2b：句柄由调用方按 pid 解析好传进来（旧实现直取 `proc().hProcess`）。 */
static size_t read_block(HANDLE hProcess, size_t addr, void *data, size_t size)
{
    DWORD old_protect;
    if (!VirtualProtectEx(hProcess, (LPVOID)addr, size, PAGE_EXECUTE_READ, &old_protect))
        return 0;
    size_t got      = 0;
    SIZE_T new_size = 0;
    if (ReadProcessMemory(hProcess, (LPVOID)addr, data, size, &new_size) && new_size <= size)
        got = (size_t)new_size;
    if (!VirtualProtectEx(hProcess, (LPVOID)addr, size, old_protect, &old_protect))
        E("VirtualProtectEx");
    return got;
}

/* 逐页读（plan_t7.md §2.2 第 12 条）：**在第一个读不了的页处停下**，返回实际读到的字节数。
 * 为什么必须逐页：`VirtualProtectEx` / `ReadProcessMemory` 都是**整段**语义 —— 256 MB 的请求里
 *   只要有一页不可读就整体失败，调用方连「已经读到的那部分是真的」都拿不到（旧 `ReadMemory`
 *   就是这样把部分读吞掉的）。逐页把失败收敛到那一块，前面的字节仍然有效。
 * 不变式：`read()` 只是它的「全有或全无」包装 ⇒ 29 个契约名（`ReadMemory` / `LoadStr` /
 *   `GetContext` 之类）的行为一个字节都没变；`bindings.cpp` 里读内存的代码一律没动。 */
size_t DebuggerSession::read_some(size_t addr, void *data, size_t size)
{
    return read_some(0, addr, data, size);   /* 0 = 根进程（旧签名原样保留，见文件头） */
}

size_t DebuggerSession::read_some(DWORD pid, size_t addr, void *data, size_t size)
{
    if (!addr || !data || !size)
    {
        LOG(TEXT("参数错误: addr: %zu, data: %p, size: %zu"), addr, data, size);
        return 0;
    }
    /* C2b：句柄按 pid 取（根 = `proc().hProcess`，子进程 = 缓存的 OpenProcess 句柄）。
     * 拿不到就是 0 字节 —— 与“一字节都读不到”同一口径，不另立错误码。 */
    HANDLE hProcess = handle_for_pid(pid);
    if (!hProcess)
        return 0;

    SYSTEM_INFO si;
    ZeroMem(si);
    GetSystemInfo(&si);
    size_t page = si.dwPageSize ? si.dwPageSize : 0x1000;

    BYTE  *out  = (BYTE *)data;
    size_t done = 0;
    while (done < size)
    {
        size_t cur    = addr + done;
        size_t inpage = page - (cur % page); /* 这一块最多到页尾 */
        size_t chunk  = size - done;
        if (chunk > inpage) chunk = inpage;
        size_t got = read_block(hProcess, cur, out + done, chunk);
        done += got;
        if (got != chunk) break; /* 第一块读不了 ⇒ 停；后面的字节一个都不碰 */
    }
    return done;
}

bool DebuggerSession::read(size_t addr, void *data, size_t size)
{
    return read(0, addr, data, size);   /* 0 = 根进程（旧签名原样保留，见文件头） */
}

bool DebuggerSession::read(DWORD pid, size_t addr, void *data, size_t size)
{
    if (!addr || !data || !size)
    {
        LOG(TEXT("参数错误: addr: %zu, data: %p, size: %zu"), addr, data, size);
        return false;
    }
    size_t got = read_some(pid, addr, data, size);
    if (got == size)
        return true;
    /* 旧实现在这里打的是「哪个系统调用失败」（`VirtualProtectEx(addr, size)` / `ReadProcessMemory`）。
     * 逐页之后那几个调用不再对应**整个**范围，所以改报真正的事实：要多少、只拿到多少。 */
    E("ReadProcessMemory(%p, %zu), only %zu", addr, size, got);
    return false;
}

bool DebuggerSession::write(size_t addr, const void *data, size_t size, bool up_intrin)
{
    return write(0, addr, data, size, up_intrin);   /* 0 = 根进程（旧签名原样保留，见文件头） */
}

bool DebuggerSession::write(DWORD pid, size_t addr, const void *data, size_t size, bool up_intrin)
{
    if (!addr || !data || !size)
    {
        LOG(TEXT("参数错误"));
        return false;
    }
    HANDLE hProcess = handle_for_pid(pid);
    if (!hProcess)
        return false;
    DWORD old_protect;
    if (!VirtualProtectEx(hProcess, (LPVOID)addr, size, PAGE_EXECUTE_READWRITE, &old_protect))
    {
        E("VirtualProtectEx");
        return false;
    }
    bool   r = true;
    SIZE_T new_size;
    if (!WriteProcessMemory(hProcess, (LPVOID)addr, data, size, &new_size))
    {
        E("WriteProcessMemory");
        r = false;
    }
    else if (size != new_size)
        r = false;
    else if (up_intrin && !FlushInstructionCache(hProcess, NULL, 0))
    {
        E("FlushInstructionCache");
        r = false;
    }
    if (!VirtualProtectEx(hProcess, (LPVOID)addr, size, old_protect, &old_protect))
        E("VirtualProtectEx");
    return r;
}

bool DebuggerSession::update_memory(size_t addr, void *old_data, const void *new_data, size_t size, bool is_up_intrin)
{
    return update_memory(0, addr, old_data, new_data, size, is_up_intrin);   /* 0 = 根进程 */
}

bool DebuggerSession::update_memory(DWORD pid, size_t addr, void *old_data, const void *new_data, size_t size, bool is_up_intrin)
{
    if (!addr || !size || (!old_data && !new_data))
    {
        LOG(TEXT("参数错误"));
        return false;
    }
    if (old_data && !read(pid, addr, old_data, size))
        return false;
    return !new_data || write(pid, addr, new_data, size, is_up_intrin);
}

/* P3.B 模块 8：从 bindings.cpp 迁入（符号表原本归 memory；`lua_load_str` 与
 * `get_cpp_class_name` 都用它）。逐字搬入，只把 `realloc` 换成 `common::resize`、
 * `memcpy` 换成 `CopyMem`（等价，见 memory.hpp 的说明）。 */
bool DebuggerSession::loadstr(size_t addr, char **pp, size_t *nn)
{
    return loadstr(0, addr, pp, nn);   /* 0 = 根进程（旧签名原样保留，见文件头） */
}

bool DebuggerSession::loadstr(DWORD pid, size_t addr, char **pp, size_t *nn)
{
    char   buf[16];
    size_t i;
    size_t n = 16;
    *nn      = 0;
    *pp      = NULL;
    do
    {
        if (!read(pid, addr, buf, sizeof buf))
            return false;
        addr += n;

        for (i = 0; i < n && buf[i]; i++);
        if (!i)
            continue;
        *pp = (char *)autodbg::common::resize(*pp, *nn + i);
        CopyMem(*pp + *nn, buf, i);
        *nn += i;
    } while (i == n);
    return true;
}

/* ============================ 内存搜索（P4.6 从绑件层下沉） ============================
 * 逐字搬自 `bindings.cpp` 的 `lua_find_memory*`（状态机 / BMH 跳表 / 错误分支全部保留）。
 * 唯一改动：把「推 Lua 栈 + `lua_error`」换成「返回枚举 + `errbuf` 文案」。
 * ❗错误分支的**副作用**（是否清零 `state`、是否释放 `buf`）也按旧实现逐个保留：
 *   · realloc 失败 → `state = 0` + 释放 buf（旧代码就是这样，下一次迭代就结束）；
 *   · 读失败且错误不是 ERROR_PARTIAL_COPY → 只记日志 + `break`（走到函数尾部统一清理）；
 *   · 读失败且错误是 ERROR_PARTIAL_COPY → **不清状态**、直接报错；
 *   · 读成功但字节数不符 → `state = 0` + 释放 buf + 报错。 */
void DebuggerSession::find_memory_init(FindMemoryContext *ctx, const void *pattern, size_t len)
{
    ctx->pattern = (unsigned char *)alloc_(len);
    CopyMem(ctx->pattern, pattern, len);
    ctx->pattern_len = len;
    for (size_t i = 0; i < 0x100; i++) ctx->shift[i] = len;
    for (size_t i = 0; i < len - 1; i++) ctx->shift[ctx->pattern[i]] = len - 1 - i;

    SYSTEM_INFO si;
    ZeroMem(si);
    GetSystemInfo(&si);
    ctx->addr       = (BYTE *)si.lpMinimumApplicationAddress;
    ctx->max_addr   = (BYTE *)si.lpMaximumApplicationAddress;
    ctx->buf        = NULL;
    ctx->buf_len    = 0;
    ctx->buf_maxlen = 0;
    ctx->state      = 1;
    ctx->pos        = 0;
    ctx->base       = 0;
}

FindMemoryKind DebuggerSession::find_memory_next(FindMemoryContext *ctx, FindMemoryMatch *out, char *errbuf, size_t errcap)
{
    while (ctx->addr < ctx->max_addr)
    {
        if (ctx->state == 1)
        {
            MEMORY_BASIC_INFORMATION mbi;
            ZeroMem(mbi);
            SIZE_T ret = VirtualQueryEx(proc().hProcess, ctx->addr, &mbi, sizeof mbi);
            if (!ret)
                break;
            ctx->base    = (size_t)mbi.BaseAddress;
            ctx->buf_len = mbi.RegionSize;
            bool b       = false;
            b            = b || mbi.Protect == PAGE_READWRITE;
            b            = b || mbi.Protect == PAGE_EXECUTE_READWRITE;
            if (!b || mbi.RegionSize < ctx->pattern_len)
            {
                ctx->state = 3;
                continue;
            }
            if (ctx->buf_maxlen < mbi.RegionSize)
            {
                /* `realloc` → `common::resize`（同 loadstr：OOM 时 fail-fast 而不是返回 NULL）。
                 * 下面的空指针分支因此实际上不会走到，**保留它只是与旧实现同形**。 */
                unsigned char *p = (unsigned char *)autodbg::common::resize(ctx->buf, mbi.RegionSize);
                if (!p)
                {
                    ctx->state = 0;
                    free_(ctx->buf);
                    ctx->buf = 0;
                    StringCchCopyA(errbuf, errcap, "out of memory");
                    return FindMemoryKind::Failed;
                }
                ctx->buf        = p;
                ctx->buf_maxlen = mbi.RegionSize;
            }
            /* C++ 下 SIZE_T ≠ size_t（x86: unsigned long vs unsigned int）→ 用 SIZE_T */
            SIZE_T n;
            if (!ReadProcessMemory(proc().hProcess, mbi.BaseAddress, ctx->buf, ctx->buf_len, &n))
            {
                if (GetLastError() != ERROR_PARTIAL_COPY)
                {
                    E("ReadProcessMemory(%p, %zu)", mbi.BaseAddress, ctx->buf_len);
                    break; /* 旧行为：只报错就停（不抛） */
                }
                wsprintfA(errbuf, "ReadProcessMemory(%p), need read %X, only read %X", mbi.BaseAddress, (DWORD)ctx->buf_len, (DWORD)n);
                return FindMemoryKind::Failed;
            }
            if (n != mbi.RegionSize)
            {
                ctx->state = 0;
                free_(ctx->buf);
                ctx->buf = 0;
                StringCchCopyA(errbuf, errcap, "ReadProcessMemory failed");
                return FindMemoryKind::Failed;
            }
            ctx->state = 2;
            ctx->pos   = ctx->pattern_len - 1;
            continue;
        }
        if (ctx->state == 2)
        {
            if (ctx->pos >= ctx->buf_len)
            {
                ctx->state = 3;
                continue;
            }
            size_t j;
            for (j = 0; j < ctx->pattern_len && ctx->pattern[ctx->pattern_len - 1 - j] == ctx->buf[ctx->pos - j]; j++);
            size_t new_pos = ctx->pos - ctx->pattern_len + 1;
            ctx->pos += ctx->shift[ctx->buf[ctx->pos]];
            if (j == ctx->pattern_len)
            {
                out->ea   = new_pos + ctx->base;
                out->data = ctx->buf + new_pos;
                out->len  = ctx->pattern_len;
                return FindMemoryKind::Match;
            }
            continue;
        }
        if (ctx->state == 3)
        {
            BYTE *next = (BYTE *)ctx->base + ctx->buf_len;
            if (next < ctx->addr)
                break;
            ctx->addr  = next;
            ctx->state = 1;
            continue;
        }
        assert(false);
    }
    ctx->state = 0;
    free_(ctx->buf);
    ctx->buf = NULL;
    return FindMemoryKind::End;
}

void DebuggerSession::find_memory_release(FindMemoryContext *ctx)
{
    if (ctx)
    {
        free_(ctx->buf);
        ctx->buf = NULL;
        free_(ctx->pattern);
        ctx->pattern = NULL;
    }
}

/* ============================ 内存区域遍历（P4.6 从绑件层下沉） ============================ */
void DebuggerSession::get_memory_region_begin(MemRegionIter *it)
{
    SYSTEM_INFO si;
    ZeroMem(si);
    GetSystemInfo(&si);
    it->addr     = (size_t)si.lpMinimumApplicationAddress;
    it->max_addr = (size_t)si.lpMaximumApplicationAddress;
}

bool DebuggerSession::get_memory_region_next(MemRegionIter *it, MemRegionInfo *out)
{
    while (it->addr < it->max_addr)
    {
        MEMORY_BASIC_INFORMATION mbi;
        ZeroMem(mbi);
        SIZE_T ret = VirtualQueryEx(proc().hProcess, (LPCVOID)it->addr, &mbi, sizeof mbi);
        if (!ret)
            break;
        out->BaseAddress       = (size_t)mbi.BaseAddress;
        out->AllocationBase    = (size_t)mbi.AllocationBase;
        out->RegionSize        = mbi.RegionSize;
        out->AllocationProtect = mbi.AllocationProtect;
        out->State             = mbi.State;
        out->Protect           = mbi.Protect;
        out->Type              = mbi.Type;

        size_t next = (size_t)mbi.BaseAddress + mbi.RegionSize;
        if (next < it->addr)
            break; /* 旧行为：指针溢出就结束 */
        it->addr = next;
        return true;
    }
    return false;
}