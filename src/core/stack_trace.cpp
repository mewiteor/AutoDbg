/* =============================================================================
 * src/core/stack_trace.cpp —— 调用栈回溯（P3.B 模块 8，逐字从 bindings.cpp 迁入）
 *
 * 内容：dbghelp `StackWalk64` 的四个回调（32/64 位各一套）+ `find_runtime_function`。
 * 跨模块改动（仅这些）：
 *   · 全部去掉 `static`（绑定层 `lua_stack_trace_iter` 要作为函数指针传下去）；
 *   · x86 的 dbghelp 兼容结构体/typedef 移到 `stack_trace.hpp`（绑定层的
 *     `StackTraceContext` 也要用）；
 *   · 其余函数体逐字未改（含 `StackTrace_TranslateAddress*` 的 `assert(false)` 桩）。
 * ❗`lua_stack_trace` / `lua_stack_trace_iter`（Lua userdata 与栈帧表构造）留在绑定层。
 * 依赖：pe（`get_peinfo_by_ea`）、dbghelp（`SymFunctionTableAccess`，x86 路径）。
 *
 * Win9x 兼容（本次改动）：
 *   · `dbghelp.dll` **不是** Win9x 自带（Win95/98/ME 都没有，需要单独分发），而手工映射
 *     的模块里导入解析失败是**硬失败**（见 module_loader）→ `SymFunctionTableAccess` /
 *     `UnDecorateSymbolName` 改为 dynapi **动态解析**：取到即用；取不到就让栈回溯降级
 *     （函数表访问返回 NULL、符号名保留修饰形式）。`StackWalk`/`StackWalk64` 本来就是动态取。
 *   · 判据：x86 产物导入表里不得再出现 dbghelp.dll（tools/api_check）。
 * ========================================================================== */
#include "stack_trace.hpp"
#include "memory.hpp" /* ReadMemory / loadstr（类名读取用） */
#include "pe.hpp"
#include "util.hpp" /* alloc_ / free_ / CopyMem 桥接宏 */
#include "common/dynapi.hpp" /* get_proc_as：dbghelp 的动态解析（Win9x 兼容） */

#include <assert.h>
#include <intrin.h> /* CopyMem 宏（__movsb），同 breakpoints.cpp / memory.cpp */

/* 类型统一（删 legacy_types.h）：这些名字现在只有 types.hpp 里的 `autodbg::core::` 那套 */
using autodbg::core::PEInfo;
using autodbg::core::DebuggerSession; /* P6.4：引擎函数已是它的成员 */

namespace
{
/* ------------------------ dbghelp 动态解析（Win9x 兼容） ------------------------
 * 调用点签名保持不变；解析结果由 dynapi 自己缓存（同名同字面量第二次直接命中），
 * 因此这里不用函数级 static 变量（零 CRT 下带动态初始化的 static 会牵进 CRT 的守卫符号）。 */
PVOID dbghelp_function_table_access(HANDLE hProcess, DWORD addrBase)
{
    typedef PVOID(WINAPI * fn_t)(HANDLE, DWORD);
    fn_t fn = autodbg::common::get_proc_as<fn_t>("dbghelp.dll", "SymFunctionTableAccess");
    return fn ? fn(hProcess, addrBase) : NULL;
}

/* 返回「写进 out 的字符数」（调用点用它定位结尾的 0）；取不到 dbghelp 时原样拷贝修饰名。 */
DWORD dbghelp_undecorate_symbol_name(PCSTR name, PSTR out, DWORD out_size)
{
    typedef DWORD(WINAPI * fn_t)(PCSTR, PSTR, DWORD, DWORD);
    fn_t  fn = autodbg::common::get_proc_as<fn_t>("dbghelp.dll", "UnDecorateSymbolName");
    if (fn)
        return fn(name, out, out_size, 0);
    DWORD i = 0;
    while (out_size && name[i] && i + 1 < out_size)
    {
        out[i] = name[i];
        i++;
    }
    if (out_size)
        out[i] = 0;
    return i;
}
} /* namespace */

/**
 * 读取进程内存的回调函数
 */
BOOL __stdcall StackTrace_ReadProcessMemory(HANDLE hProcess, DWORD64 qwBaseAddress, PVOID lpBuffer, DWORD nSize, LPDWORD lpNumberOfBytesRead)
{
    SIZE_T bytesRead;
    BOOL   result = ReadProcessMemory(hProcess, (LPCVOID)(ULONG_PTR)qwBaseAddress, lpBuffer, nSize, &bytesRead);
    if (lpNumberOfBytesRead)
        *lpNumberOfBytesRead = (DWORD)bytesRead;
    return result;
}
#ifndef _WIN64
BOOL __stdcall StackTrace_ReadProcessMemory32(HANDLE hProcess, DWORD lpBaseAddress, PVOID lpBuffer, DWORD nSize, PDWORD lpNumberOfBytesRead)
{
    SIZE_T bytesRead;
    BOOL   result = ReadProcessMemory(hProcess, (LPCVOID)lpBaseAddress, lpBuffer, nSize, &bytesRead);
    if (lpNumberOfBytesRead)
        *lpNumberOfBytesRead = bytesRead;
    return result;
}
#else
PVOID find_runtime_function(PEInfo *modinfo, DWORD rva, bool resolveIndirect)
{
    if (!modinfo->runtime_functions || !modinfo->runtime_function_count)
        return NULL;

    int lo = 0, hi = modinfo->runtime_function_count - 1;
    while (lo <= hi)
    {
        int               mid = lo + (hi - lo) / 2;
        PRUNTIME_FUNCTION rf  = &modinfo->runtime_functions[mid];
        if (rva < rf->BeginAddress)
            hi = mid - 1;
        else if (rva >= rf->EndAddress)
            lo = mid + 1;
        else
        {
            if (resolveIndirect && (rf->UnwindData & 1))
                return find_runtime_function(modinfo, rf->UnwindData & ~1, false);
            return modinfo->runtime_functions + mid;
        }
    }
    return NULL;
}
#endif

/**
 * 函数表访问回调 - 从debugger.lua functionTableAccessRoutine64移植
 * 在64位下查找PE运行时函数表，32位下委托给dbghelp
 */
#ifdef _WIN64
PVOID __stdcall StackTrace_FunctionTableAccess(HANDLE hProcess, DWORD64 addrBase)
{
    PEInfo *peinfo = autodbg::core::current_session().get_peinfo_by_ea(addrBase);
    if (!peinfo || !peinfo->base)
        return NULL;
    return find_runtime_function(peinfo, (DWORD)(addrBase - peinfo->base), true);
}
#else
PVOID __stdcall StackTrace_FunctionTableAccess32(HANDLE hProcess, DWORD addrBase)
{
    /* 动态解析：Win9x 没有 dbghelp.dll → 返回 NULL，栈回溯降级 */
    return dbghelp_function_table_access(hProcess, addrBase);
}

/* D27 修复：`StackWalk64`（dbghelp）在 x86 上的回调是 64 位风格地址 → 适配到同一个实现。 */
PVOID __stdcall StackTrace_FunctionTableAccess64(HANDLE hProcess, DWORD64 addrBase)
{
    return dbghelp_function_table_access(hProcess, (DWORD)addrBase);
}
#endif

/**
 * 获取模块基址回调
 */
DWORD64 __stdcall StackTrace_GetModuleBase(HANDLE hProcess, DWORD64 addrBase)
{
    // 简化实现，返回0让系统处理
    PEInfo *peinfo = autodbg::core::current_session().get_peinfo_by_ea((size_t)addrBase);
    if (peinfo)
        return peinfo->base;
    return 0;
}

#ifndef _WIN64
DWORD __stdcall StackTrace_GetModuleBase32(HANDLE hProcess, DWORD addrBase)
{
    PEInfo *peinfo = autodbg::core::current_session().get_peinfo_by_ea(addrBase);
    if (peinfo)
        return peinfo->base;
    return 0;
}
#endif

/**
 * 地址转换回调
 */
DWORD64 __stdcall StackTrace_TranslateAddress(HANDLE hProcess, HANDLE hThread, LPADDRESS64 addr)
{
    assert(false);
    return 0;
}

#ifndef _WIN64
DWORD __stdcall StackTrace_TranslateAddress32(HANDLE hProcess, HANDLE hThread, LPADDRESS_ addr)
{
    assert(false);
    return 0;
}
#endif

/* -----------------------------------------------------------------------------
 * 异常描述表 + C++ 类名读取（P3.B 模块 8 的「去 Lua 化」部分）
 *
 * 来源：`scripting/lua/bindings.cpp` 的 `DumpExceptionRecord2Lua` / `DumpCppClassInfo2Lua`。
 * 这两处原本直接往 Lua 表里塞值；现在引擎只给**数据**，绑定层负责建表：
 *   · `describe_exception_code`      ：异常码 → 名字 / 中文说明（整张表**脚本化抽取**，未手抄）；
 *   · `exception_access_type_name`   ：访问违例/页内错误的第一参数 → 操作类型名；
 *   · `get_cpp_class_name`           ：读目标内存里的 C++ 类信息并去修饰。
 * ❗字段名/嵌套结构与中文文案必须与旧实现逐字一致 → `tests/golden/exception_*.txt` 是裁判。
 * -------------------------------------------------------------------------- */

void describe_exception_code(DWORD code, const char **name, const char **info)
{
    *name = nullptr;
    *info = nullptr;
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:
        *name = "ACCESS_VIOLATION";
        *info = "线程尝试从虚拟地址读取或写入其没有相应访问权限的虚拟地址。";
        break;
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        *name = "ARRAY_BOUNDS_EXCEEDED";
        *info = "线程尝试访问超出边界且基础硬件支持边界检查的数组元素。";
        break;
    case EXCEPTION_BREAKPOINT:
        *name = "BREAKPOINT";
        *info = "遇到断点。";
        break;
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        *name = "DATATYPE_MISALIGNMENT";
        *info = "线程尝试读取或写入在不提供对齐的硬件上未对齐的数据。 例如，16 位值必须在 2字节边界上对齐;4 字节边界上的 32 位值等。";
        break;
    case EXCEPTION_FLT_DENORMAL_OPERAND:
        *name = "FLT_DENORMAL_OPERAND";
        *info = "浮点运算中的一个操作数是反常运算。 非规范值太小，无法表示为标准浮点值。";
        break;
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        *name = "FLT_DIVIDE_BY_ZERO";
        *info = "线程尝试将浮点值除以 0 的浮点除数。";
        break;
    case EXCEPTION_FLT_INEXACT_RESULT:
        *name = "FLT_INEXACT_RESULT";
        *info = "浮点运算的结果不能完全表示为小数点。";
        break;
    case EXCEPTION_FLT_INVALID_OPERATION:
        *name = "FLT_INVALID_OPERATION";
        *info = "此异常表示此列表中未包含的任何浮点异常。";
        break;
    case EXCEPTION_FLT_OVERFLOW:
        *name = "FLT_OVERFLOW";
        *info = "浮点运算的指数大于相应类型允许的量级。";
        break;
    case EXCEPTION_FLT_STACK_CHECK:
        *name = "FLT_STACK_CHECK";
        *info = "堆栈因浮点运算而溢出或下溢。";
        break;
    case EXCEPTION_FLT_UNDERFLOW:
        *name = "FLT_UNDERFLOW";
        *info = "浮点运算的指数小于相应类型允许的量级。";
        break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        *name = "ILLEGAL_INSTRUCTION";
        *info = "线程尝试执行无效指令。";
        break;
    case EXCEPTION_IN_PAGE_ERROR:
        *name = "IN_PAGE_ERROR";
        *info = "线程尝试访问不存在的页面，但系统无法加载该页。例如，如果在通过网络运行程序时网络连接断开，则可能会发生此异常。";
        break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        *name = "INT_DIVIDE_BY_ZERO";
        *info = "线程尝试将整数值除以零的整数除数。";
        break;
    case EXCEPTION_INT_OVERFLOW:
        *name = "INT_OVERFLOW";
        *info = "整数运算的结果导致执行结果中最重要的位。";
        break;
    case EXCEPTION_INVALID_DISPOSITION:
        *name = "INVALID_DISPOSITION";
        *info = "异常处理程序向异常调度程序返回了无效处置。 使用高级语言（如C）的程序员不应遇到此异常。";
        break;
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
        *name = "NONCONTINUABLE_EXCEPTION";
        *info = "线程尝试在发生不可连续的异常后继续执行。";
        break;
    case EXCEPTION_PRIV_INSTRUCTION:
        *name = "PRIV_INSTRUCTION";
        *info = "线程尝试执行在当前计算机模式下不允许其操作的指令。";
        break;
    case EXCEPTION_SINGLE_STEP:
        *name = "SINGLE_STEP";
        *info = "跟踪陷阱或其他单指令机制指示已执行一个指令。";
        break;
    case EXCEPTION_STACK_OVERFLOW:
        *name = "STACK_OVERFLOW";
        *info = "线程占用了其堆栈。";
        break;
    case 0xE06D7363:
        *name = "CPP_EH_EXCEPTION";
        *info = "c++结构化异常。";
        break;
    }
}

/* 访问违例 / 页内错误的第一参数（操作类型）→ 名字；未知类型返回 nullptr。
 * 旧实现在两个 case 里各写了一遍同样的 switch（推字符串 0/1/8、其余推数字）→ 收敛到这里。
 * 注：文案与旧实现逐字一致（含 “用户模式数据执行防护 (DEP)” 里的空格与半角括号）。 */
const char *exception_access_type_name(ULONG_PTR type)
{
    switch (type)
    {
    case 0 : return "读取";
    case 1 : return "写入";
    case 8 : return "用户模式数据执行防护 (DEP)";
    default: return nullptr;
    }
}

/* 读取 `addr` 处的 C++ 类名：vtable →（addr-8）→ 类型描述符 → 名字字符串 → 去修饰。
 * 逐字搬自 `DumpCppClassInfo2Lua`，只做三处替换：
 *   · `malloc`/`free` → `alloc_`/`free_`（等价桥接，见 util.hpp）；
 *   · `memcpy` → `CopyMem`（等价）；
 *   · 末尾的 `lua_pushstring(L, p)` → 拷进调用方缓冲 `out`（旧实现随即 free 掉源缓冲，
 *     字符串只在 push 那一刻有效）。
 * 返回 false 表示读内存失败（= 旧实现返回 false）；`has_name` 对应旧实现
 *   “`n > 3` 才会产出名字”的条件 —— **1:1 保留**：n <= 3 时返回 true 但无输出。
 * `cap` 应给足（旧实现按 0x1000 分配）；不足时会截断（仅此一处新行为，调用方按 0x1000 传）。 */
bool DebuggerSession::get_cpp_class_name(size_t addr, char *out, size_t cap, bool *has_name)
{
    if (has_name)
        *has_name = false;
    if (!addr)
        return false;
    if (!read(addr, &addr, sizeof addr))
        return false;
    if (!addr)
        return false;
    if (!read(addr - 8, &addr, sizeof addr))
        return false;
    if (!addr)
        return false;
    uint32_t a;
    if (!read(addr, &a, sizeof a))
        return false;
    if (!a)
        return false;
    if (!read(addr + 12, &a, sizeof a))
        return false;
    uint32_t b;
    if (!read(addr + 20, &b, sizeof b))
        return false;
    char  *p;
    size_t n;
    if (!loadstr(addr + a - b + 0x10, &p, &n))
        return false;
    if (n > 3)
    {
        char *buf = (char *)alloc_(n - 2);
        CopyMem(buf, p + 3, n - 3);
        buf[0]     = '?';
        buf[n - 3] = 0;
        char *buf2                                       = (char *)alloc_(0x1000);
        buf2[dbghelp_undecorate_symbol_name(buf, buf2, 0x1000)] = 0;
        char *q                                          = buf2;
        while (*q && (*q == ' ' || *q == '?')) q++;
        if (cap)
        {
            size_t i = 0;
            while (q[i] && i + 1 < cap)
            {
                out[i] = q[i];
                i++;
            }
            out[i] = 0;
        }
        if (has_name)
            *has_name = true;
        free_(buf);
        free_(buf2);
    }
    if (p)
        free_(p);
    return true;
}
