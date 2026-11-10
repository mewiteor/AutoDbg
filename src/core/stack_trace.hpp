#ifndef AUTODBG_CORE_STACK_TRACE_HPP
#define AUTODBG_CORE_STACK_TRACE_HPP
/* =============================================================================
 * src/core/stack_trace.hpp —— 调用栈回溯（P3.B 模块 8）
 *
 * 从 `scripting/lua/bindings.cpp` 迁入（P3.B 模块 8），与旧实现**逐字一致**：
 *   · `StackTrace_ReadProcessMemory(_32)` / `StackTrace_FunctionTableAccess(_32)` /
 *     `StackTrace_GetModuleBase(_32)` / `StackTrace_TranslateAddress(_32)` —— dbghelp
 *     `StackWalk64` 的四个回调；
 *   · `find_runtime_function`（x64：在 PE 的 RUNTIME_FUNCTION 表里二分查找）。
 *   `lua_stack_trace` / `lua_stack_trace_iter`（迭代器与栈帧表构造）**留在绑定层**
 *   —— 它们直接操作 Lua 栈与 `StackTraceContext`（Lua userdata）。
 *
 * ❗x86 兼容结构体（`ADDRESS_`/`KDHELP_`/`STACKFRAME_` 与对应函数指针 typedef）：
 *   原来定义在 `bindings.cpp` 的 `#ifndef _WIN64` 块里。绑定层的 `StackTraceContext`
 *   也用到它们 → 移到本头文件共享，**定义逐字未改**（`#ifndef _WIN64` 同样保留）。
 *
 * 依赖：dbghelp（`ADDRESS_MODE` / `*_ROUTINE64` / `STACKFRAME64` 等类型）、
 *       pe（`get_peinfo_by_ea`）、util（ZeroMem 等）。
 * ========================================================================== */
#include <windows.h>
#include <dbghelp.h> /* ADDRESS_MODE / PREAD_PROCESS_MEMORY_ROUTINE64 / STACKFRAME64 … */

#include "core.hpp"   /* P6.4：引擎函数已是 DebuggerSession 成员（下面的过渡转发也要它） */
#include "types.hpp" /* PEInfo */

#ifndef _WIN64
/* ---------------- x86 专用：dbghelp 的 32 位兼容结构体 ----------------
 * 逐字从 bindings.cpp 迁入（原在 `#ifndef _WIN64` 块里）。绑定层也用它
 *   定义 Lua userdata `StackTraceContext`。 */
typedef struct
{
    DWORD        Offset;
    WORD         Segment;
    ADDRESS_MODE Mode;
} ADDRESS_, *LPADDRESS_;

typedef struct
{
    DWORD Thread;
    DWORD ThCallbackStack;
    DWORD NextCallback;
    DWORD FramePointer;
    DWORD KiCallUserMode;
    DWORD KeUserCallbackDispatcher;
    DWORD SystemRangeStart;
    DWORD ThCallbackBStore;
    DWORD KiUserExceptionDispatcher;
    DWORD StackBase;
    DWORD StackLimit;
    DWORD Reserved[5];
} KDHELP_, *PKDHELP_;

typedef struct
{
    ADDRESS_ AddrPC;         // program counter
    ADDRESS_ AddrReturn;     // return address
    ADDRESS_ AddrFrame;      // frame pointer
    ADDRESS_ AddrStack;      // stack pointer
    PVOID    FuncTableEntry; // pointer to pdata/fpo or NULL
    DWORD    Params[4];      // possible arguments to the function
    BOOL     Far;            // WOW far call
    BOOL     Virtual;        // is this a virtual frame?
    DWORD    Reserved[3];
    KDHELP_  KdHelp;
    ADDRESS_ AddrBStore; // backing store pointer
} STACKFRAME_, *LPSTACKFRAME_;

typedef BOOL(__stdcall *PREAD_PROCESS_MEMORY_ROUTINE_)(HANDLE, DWORD, PVOID, DWORD, PDWORD);
typedef PVOID(__stdcall *PFUNCTION_TABLE_ACCESS_ROUTINE_)(HANDLE, DWORD);
typedef DWORD(__stdcall *PGET_MODULE_BASE_ROUTINE_)(HANDLE, DWORD);
typedef DWORD(__stdcall *PTRANSLATE_ADDRESS_ROUTINE_)(HANDLE, HANDLE, LPADDRESS_);
typedef BOOL(__stdcall *StackWalkT)(DWORD, HANDLE, HANDLE, LPSTACKFRAME_, PVOID, PREAD_PROCESS_MEMORY_ROUTINE_, PFUNCTION_TABLE_ACCESS_ROUTINE_,
                                    PGET_MODULE_BASE_ROUTINE_, PTRANSLATE_ADDRESS_ROUTINE_);
#endif /* !_WIN64 */

/* `StackWalk64` 的签名在 x86 / x64 上一样（都是 64 位风格）→ typedef 两只架构共用，
 * 因为 x64 也改为**动态解析**了（去掉 dbghelp.dll 静态导入，见 stack_trace.cpp 头注释）。 */
typedef BOOL(__stdcall *StackWalk64T)(DWORD, HANDLE, HANDLE, LPSTACKFRAME64, PVOID, PREAD_PROCESS_MEMORY_ROUTINE64, PFUNCTION_TABLE_ACCESS_ROUTINE64,
                                      PGET_MODULE_BASE_ROUTINE64, PTRANSLATE_ADDRESS_ROUTINE64);

/* ---------------- dbghelp 回调（签名必须与 dbghelp 一致） ---------------- */

/* 把目标内存读进本地缓冲：x64 为 64 位地址变体，x86 为 32 位地址变体 */
BOOL __stdcall StackTrace_ReadProcessMemory(HANDLE hProcess, DWORD64 qwBaseAddress, PVOID lpBuffer, DWORD nSize, LPDWORD lpNumberOfBytesRead);
#ifndef _WIN64
BOOL __stdcall StackTrace_ReadProcessMemory32(HANDLE hProcess, DWORD lpBaseAddress, PVOID lpBuffer, DWORD nSize, PDWORD lpNumberOfBytesRead);
#endif

/* x64：PE RUNTIME_FUNCTION 表查找（resolveIndirect 处理间接 unwind 条目） */
#ifdef _WIN64
PVOID find_runtime_function(autodbg::core::PEInfo *modinfo, DWORD rva, bool resolveIndirect);
#endif

/* 函数表访问：x64 查 PE 的 RUNTIME_FUNCTION；x86 委托 dbghelp 的 SymFunctionTableAccess */
#ifdef _WIN64
PVOID __stdcall StackTrace_FunctionTableAccess(HANDLE hProcess, DWORD64 addrBase);
#else
PVOID __stdcall StackTrace_FunctionTableAccess32(HANDLE hProcess, DWORD addrBase);
/* ❗D27 修复：x86 上改用 `StackWalk64`（回调签名是 **64 位风格**；`StackWalk64` 在 dbghelp.dll，
 * 旧代码从 kernel32 取 → 恒 NULL → 调空指针自杀）→ 这里补一个 64 位风格的适配。 */
PVOID __stdcall StackTrace_FunctionTableAccess64(HANDLE hProcess, DWORD64 addrBase);
#endif

/* 模块基址（查已知模块表；查不到返回 0 让 dbghelp 自己处理） */
DWORD64 __stdcall StackTrace_GetModuleBase(HANDLE hProcess, DWORD64 addrBase);
#ifndef _WIN64
DWORD __stdcall StackTrace_GetModuleBase32(HANDLE hProcess, DWORD addrBase);
#endif

/* 地址转换：**未实现**（旧实现就是 `assert(false); return 0;`，1:1 保留） */
DWORD64 __stdcall StackTrace_TranslateAddress(HANDLE hProcess, HANDLE hThread, LPADDRESS64 addr);
#ifndef _WIN64
DWORD __stdcall StackTrace_TranslateAddress32(HANDLE hProcess, HANDLE hThread, LPADDRESS_ addr);
#endif

/* ---------------- 异常 / C++ 类信息（模块 8 的去 Lua 化部分） ---------------- */

/* 异常码 → 名字 / 中文说明（均为静态字符串）。
 * ❗未知码把两者都置为 nullptr —— 与旧实现一致（旧代码不设 `code_name`/`code_info` 字段），
 *   调用方必须据此决定“不设字段”，否则会改变 Lua 侧表的形状（黄金是裁判）。 */
void describe_exception_code(DWORD code, const char **name, const char **info);

/* 访问违例 / 页内错误的第一参数（操作类型）→ 名字；未知类型返回 nullptr（旧实现改为推数字）。 */
const char *exception_access_type_name(ULONG_PTR type);

/* 读目标内存里的 C++ 类名（vtable → RTTI → 类型描述符），去修饰后写入 `out`（非空则 NUL 结尾）。
 * 返回 false = 读内存失败；`has_name` = 旧实现里“名字长度 > 3 才产出”的条件
 *   （**1:1 保留**：n <= 3 时返回 true 但 `has_name=false`、`out` 不动）。
 * `cap` 建议 0x1000（旧实现就是按这个尺寸分配的）。 */



#endif /* AUTODBG_CORE_STACK_TRACE_HPP */
