/* =============================================================================
 * src/core/debug_loop.cpp —— 调试事件主循环（P3.B 模块 9，逐字从 bindings.cpp 迁入）
 *
 * 内容（T7.1，plan_t7.md §2.1）：状态机 `begin` / `pump` / `cont` / `end`（一次 wait + 派发 +
 *       可选 continue）、`run`（= `begin` + `while` + `pump(autocont = true)` + `end` 的封装，
 *       语义与逐字节输出不变）、`run_shutdown`，以及 ScyllaHide 的条件引入（98 构建用空实现）。
 * 跨模块改动（仅这些）：
 *   · `run` 去掉 `static`（`lua_Run` / `lua_Attach` 调用）；
 *   · T7.1：`run` 只是封装，不是重写 —— 原循环里留在栈上的跨迭代状态搬成**会话成员**
 *     （`m_first_bp` / `m_push_fb` / `m_reset_bpx` / `m_reset_hw_bp` / `m_first_create_process` /
 *     `m_debug_active_process_stop`）：手工映射的模块禁止 `thread_local` 与函数内非平凡构造的
 *     static（§11.5、PITFALLS M10），搬漏一个就是跨 pump 状态丢失。
 *     `pump` 的 `autocont = false` ⇒ ❗不调 `ContinueDebugEvent`（事件停在未应答状态，由调用方
 *     显式 `cont(pid, tid, code)`）；`pump(0, false)` 即 `pump_now()`。
 *     旧循环的两处 `break`（detach 之后、`ContinueDebugEvent` 失败）改成置 `cont()` 标志 —— 对
 *     `run()` 完全等价（while 出口相同），`pump` 自己不回传「停不停」。
 *     返回值口径：`begin` 0 = 就绪 / -1 = 内部错误（收尾已跑）/ -2 = 附加失败（1:1：打印后直接
 *     结束、**不跑收尾**，由 `run` 映射回 0）；`pump` = 调试事件码（0 = 没等到事件）或 -1；`end` = 0。
 *   · `run_shutdown` 是 `run` 的私有成员（P6.4：它要用会话自身的状态）；
 *   · ScyllaHide 条件块从 bindings.cpp 整体搬来（98 构建的 `NO_SCYLLA_HIDE` 空实现）；
 *   · `lua_on_*` 适配器 / `LuaDebugAdapter` / `pcall` 仍留绑定层（宿主适配）。
 * ❗错误不用长跳：内部错误走「返回 -1 + *err」（D18），绑定层在 run 返回后 lua_error。
 * ❗空闲轮询（Q27）：`WaitForDebugEvent` 超时时调 `DebugCallbacks::on_wait_timeout`，
 *   引擎只执行结论（继续等 / 分离并终止 / 分离但保留目标）。
 * 依赖：memory / pe / breakpoints / hardware_breakpoints / process / util / engine(全局量)。
 * ========================================================================== */
#include "debug_loop.hpp"

#include "breakpoints.hpp"
#include "../common/handle.hpp" /* P6.1：HANDLE RAII */
#include "core.hpp" /* P6.4：引擎函数是会话成员（本 TU 多数定义即成员，直接用自身状态） */
#include "hardware_breakpoints.hpp"
#include "types.hpp"
#include "memory.hpp"
#include "pe.hpp"
#include "process.hpp"
#include "util.hpp"

#include <assert.h>
#include <intrin.h>
#include <psapi.h>
#include <shlwapi.h>
#include <string.h>

/* 类型统一（删 legacy_types.h）：这些名字现在只有 types.hpp 里的 `autodbg::core::` 那套 */
using autodbg::core::PEInfo;
using autodbg::core::ThreadInfo;
using autodbg::core::BreakpointInfo;

namespace core = autodbg::core;   /* 与其它 core 模块一致：显式限定，避免与 C 头同名类型二义 */
using autodbg::core::DebuggerSession; /* P6.4：引擎函数已是它的成员 */

/* 迁移期便利：旧调用点（PrintN / E 宏 / alloc_ / free_ / ZeroMem / AllocX）不带限定名。
 * E / alloc_ / free_ / ZeroMem / AllocX 是宏，无需 using；函数才需要。 */
using autodbg::core::PrintN;
using autodbg::common::unique_handle; /* P6.1 */
using autodbg::core::calc_crc32;
using autodbg::core::LjDebugSetProcessKillOnExit;
#ifndef _UNICODE
/* 这个只在非 Unicode 构建存在（ANSI 专属 shim），Unicode 下 using 会报 C2039 */
using autodbg::core::LjGetMappedFileNameA;
#endif


#ifdef NO_SCYLLA_HIDE
/* --98=y (Win9x) builds do not use ScyllaHide (xmake.lua: NO_SCYLLA_HIDE):
 *   1) Win9x has none of the anti-debug techniques this library counters;
 *   2) the library (and its 3rdparty/ntdll import lib) pulls in a lot of XP+
 *      imports (Nt, Rtl, Ldr, GetNativeSystemInfo, ...), which conflicts with
 *      the "no XP+ API in x86 products" goal; see tests/tools/audit_imports.ps1.
 * The stubs below keep the six call sites and the use_scylla_hide option intact
 * (the option is still accepted, it just has no effect). Our own anti-anti-debug
 * code (hide.cpp: fix_peb / patch_antidebug_api) is unaffected.
 * NOTE: this file is CP936-encoded, so this block is intentionally ASCII-only. */
static void scylla_hide_setup(void) {}
static void scylla_hide_on_stop(void) {}
static void scylla_hide_on_debug(void) {}
static void scylla_hide_on_create_process(DWORD pid, void* start_address)
{
    (void)pid;
    (void)start_address;
}
static void scylla_hide_on_create_dll(void* base)
{
    (void)base;
}
static void scylla_hide_on_breakpoint(void) {}
#else
#   include <ScyllaHide.h>
#endif


void DebuggerSession::run_shutdown(const core::RunParams *params)
{
    if (proc().hProcess)
    {
        if (term())
        {
            TerminateProcess(proc().hProcess, 0);
            WaitForSingleObject(proc().hProcess, 100);
        }
        CloseHandle(proc().hProcess);
    }
    if (proc().hThread)
    {
        if (term())
        {
            TerminateThread(proc().hThread, 0);
            WaitForSingleObject(proc().hThread, 100);
        }
        CloseHandle(proc().hThread);
    }
    if (params->use_scylla_hide)
        scylla_hide_on_stop();
}

/* begin：Attach / CreateProcess + 跨 pump 的会话状态（不循环）。
 * 返回值：0 = 就绪（可进 pump 循环）；-2 = 附加失败（1:1 保留旧行为：打印后直接结束、**不跑收尾**，
 * 由 `run` 映射回 0）；-1 = 引擎内部错误（`*err` 已置，收尾已跑）。
 * ❗ 错误**不用长跳**表达：旧实现在此直接 lua_error（会跨 C++ 帧 longjmp，见 D18），
 * 现改为正常返回，由绑定层在 run 返回后 lua_error（Lua 可见行为一致）。 */
int DebuggerSession::begin(const core::RunParams *params, const core::DebugCallbacks *cb, const char **err)
{
    m_params = params;   /* pump / end 从这里取调用方对象（与 begin 的调用期同寿命） */
    m_cb     = cb;

    if (params->use_scylla_hide)
        scylla_hide_setup();

    if (params->pid)
    {
        if (!DebugActiveProcess(params->pid))
        {
            E("DebugActiveProcess");
            return -2; /* 1:1：旧实现就是直接返回（不报 Lua 错误、不跑收尾）→ run 映射回 0 */
        }
        LjDebugSetProcessKillOnExit(FALSE);
        proc().dwProcessId = params->pid;
    }
    else
    {
        DWORD flags = DEBUG_PROCESS;
        if (params->is_new_console)
            flags |= CREATE_NEW_CONSOLE;
        STARTUPINFO startup_info;
        ZeroMem(startup_info);
        ZeroMem(proc());
        startup_info.cb = sizeof startup_info;

        /* T1（D50）：`--json` 下把目标的 stdout/stderr 指到**我们自己 stderr 的可继承副本**
         * （否则目标继承我们的句柄，`cmd /c ping` 的输出会直接顶在那一行 JSON 前面）。
         * 为什么不用匿名管道：管道必须有人持续排空，而 `--detach` 之后没人能排空 ——
         * 关读端 → 目标写 stdout 拿到 `ERROR_BROKEN_PIPE`；不关不读 → 填满 4 KB 后阻塞。
         * 两者都违背 D47（detach 后目标继续活着）。副本句柄没有这些副作用：本进程关掉
         * 自己那份之后，目标持有的那份仍然有效。详见 `util.hpp` 的注释。 */
        HANDLE child_out_dup = NULL;
        HANDLE child_in_nul  = NULL;
        BOOL   inherit       = FALSE;
        if (child_stdio_capture())
        {
            HANDLE parent_err = GetStdHandle(STD_ERROR_HANDLE);
            SECURITY_ATTRIBUTES sa;
            sa.nLength              = sizeof sa;
            sa.lpSecurityDescriptor = NULL;
            sa.bInheritHandle       = TRUE;

            if (parent_err != NULL && parent_err != INVALID_HANDLE_VALUE)
                DuplicateHandle(GetCurrentProcess(), parent_err, GetCurrentProcess(), &child_out_dup,
                                0, TRUE /* 可继承 */, DUPLICATE_SAME_ACCESS);
            /* stdin 给 NUL：目标不该读掉我们的输入；输入重定向失败就不启用（宁可旧行为） */
            child_in_nul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                       OPEN_EXISTING, 0, NULL);
            if (child_in_nul == INVALID_HANDLE_VALUE)
                child_in_nul = NULL;

            if (child_out_dup != NULL && child_in_nul != NULL)
            {
                startup_info.dwFlags    = STARTF_USESTDHANDLES;
                startup_info.hStdInput  = child_in_nul;
                startup_info.hStdOutput = child_out_dup;
                startup_info.hStdError  = child_out_dup;
                inherit                 = TRUE;
            }
        }

        BOOL r          = CreateProcess(params->imagepath, (LPTSTR)params->cmdline, NULL, NULL, inherit, flags, params->env, params->cwd, &startup_info, &proc());
        /* 父进程立刻放掉自己那份副本：目标已经拿到自己的句柄，留着只会让它晚一点收到 EOF */
        if (child_out_dup != NULL)
            CloseHandle(child_out_dup);
        if (child_in_nul != NULL)
            CloseHandle(child_in_nul);
        if (!r)
            E("CreateProcess");
    }
    /* 跨 pump 的循环状态（会话成员）——每次 begin 重新置初值（原来这些是 run 的栈局部） */
    cont()                 = true;
    term()                 = true;
    to_detach()            = false;
    m_first_bp             = true;  /* 原 `FirstBPX` */
    m_push_fb              = false; /* 原 `pushfbpx`（pushf 特殊情况） */
    m_first_create_process = false; /* 旧 `dbgfilehandle` 实际只当「首个 CREATE_PROCESS」标志用 */

    /* ❗D32 修复：必须显式初始化（留在构造函数或 begin）—— 否则 `m_reset_bpx.modname` 是垃圾，
     * 会让「首个单步事件」误入下面的 re-arm 分支：① 硬件断点的首个命中会被静默丢掉；
     * ② 还会拿垃圾指针去调 `enable_breakpoint()`（debug 构建下先撞 assert）。
     * 这就是「武装后的首次访问必定漏掉」的根因（栈上时垃圾值随栈内容变化 → 早先还表现为不稳定）；
     * 搬成成员后必须**每次 begin 重新清**，否则上一轮 run 的待 re-arm 状态会漏到下一轮。 */
    m_reset_bpx.modname = NULL;
    m_reset_bpx.rva     = 0;
    m_reset_bpx.ea      = 0;
    m_reset_hw_bp       = 0; /* 原 `reset_hwbpx`：DR0-3 的位掩码 */

    /* `DebugActiveProcessStop` 在 begin 里解析一次（原来每次 run 在栈上解析）：取不到 → D47 的降级
     * （只能 terminate；`on_wait_timeout` 拿到的 can_detach_without_term = false，`end` 的返回值也据此上报） */
    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    m_debug_active_process_stop = (DebugActiveProcessStopFn)GetProcAddress(hKernel32, "DebugActiveProcessStop");

    return 0;
}

/* pump 的等待切片上限（T7.3 / plan_t7.md §4.4）：`run()` 传 100 ms ⇒ 正好一片 ⇒ 行为逐字节不变。 */
const DWORD kPumpSliceMs = 250;

/* wait_one（T7 切片 C2a / plan_t7.md §10、D57）：只做「等一条事件」，**不分派**。
 * 返回：> 0 = `dwDebugEventCode`（`*out` 已填）；0 = 没等到事件（`timeout_ms` 用尽）；
 *       -1 = 引擎内部错误（`*err` 已置）—— 现在的等待循环没有这条路径，-1 是与 `pump` 同口径的保留值。
 * ❗`WaitForDebugEvent` 是**进程级**的：本函数不按 `params->pid` 过滤，所以在**任意**会话实例上调
 *   都能读到这条进程的事件流里的事件。多会话（C2a）的进程级路由就靠这一点 —— 宿主拿任意会话去等，
 *   再按 `ev.dwProcessId` 找归属会话分派。
 * ❗detach / 取消那几条分支作用于 **this**（会话自己的 `proc()` / `m_debug_active_process_stop` /
 *   `term()` / `to_detach()`），与拆分前逐字一致。 */
int DebuggerSession::wait_one(DWORD timeout_ms, DEBUG_EVENT *out, const char **err)
{
    const DebugCallbacks *cb = m_cb;
    (void)err;   /* 与 `pump` 同签：等待循环本身还不产生引擎错误（见上面的返回值说明） */

    bool detached = false;

    /* T7.3：等待按 ≤ kPumpSliceMs 切片，每片没等到事件就问一次 `on_wait_timeout` —— 那是执行期
     * 唯一的 control poll 点（D32 / S13），所以 `--cancel` 能在 pump 空闲时插话。core 仍然只拿到
     * 一个 timeout，不知道 socket / stdin（§5 第 5 条）。 */
    for (DWORD left = timeout_ms; ; )
    {
        DWORD slice = (timeout_ms == 0) ? 0 : (left > kPumpSliceMs ? kPumpSliceMs : left);
        if (WaitForDebugEvent(out, slice))
            return (int)out->dwDebugEventCode;
        if (to_detach())
        {
            term() = false;
            detach_process(cb);
            detached = true;
        }
        else if (cb->on_wait_timeout)
        {
            switch (cb->on_wait_timeout(cb->context, !!m_debug_active_process_stop))
            {
            case core::WAIT_TIMEOUT_DETACH_KEEP_ALIVE: /* 2 */
                term() = false;
                /* fallthrough：1:1 保留旧实现的 case 2 → case 1 */
            case core::WAIT_TIMEOUT_DETACH_AND_TERMINATE: /* 1 */
                detach_process(cb);
                detached = true;
            }
        }
        if (detached)
        {
            if (m_debug_active_process_stop)
                m_debug_active_process_stop(proc().dwProcessId);
            to_detach() = false; /* detach 已经做完：别让 `end()` 再做一遍 */
            m_cont = false;      /* 旧的 `break`：run 的 while 出口（pump 不回传「停不停」，只置标志） */
            break;
        }
        if (left <= slice) break; /* timeout 用尽 ⇒ 等价于旧的 `continue`（回到 while 顶） */
        left -= slice;
    }
    return 0;               /* 0 = 没等到事件（与旧的 `continue` 同义；detach 出口也走这里） */
}

/* dispatch（C2a）：`wait_one` 之后的那一整段 —— 回调 → ScyllaHide → 按事件码的 switch → 记账 →
 * `autocont` 时 continue。返回调试事件码；-1 = 引擎内部错误（文案进 `m_last_error`，收尾已在这条路径里跑过）。
 * ❗分派**不能推迟**（D57）：`cont` 必须作用在已 EIP--/已 re-arm/已记录的事件上，所以事件一从
 *   OS 读出来就必须立刻分派 —— 多会话能做到的只是「`d:pump` 不**返回**别人的事件」。
 * ❗`autocont = false` ⇒ 不调 `ContinueDebugEvent`：事件停在未应答状态，由调用方显式
 *   `cont(pid, tid, code)`。
 * ❗形参就叫 `DBGEvent`：下面这段 switch 的正文与拆分前**逐字节相同**（拆分的判据之一是
 *   `run()` 与既有 `pump.*` 的可观察行为不变），改名只会让对照变模糊。 */
int DebuggerSession::dispatch(const DEBUG_EVENT &DBGEvent, bool autocont)
{
    const RunParams      *params = m_params;   /* begin 存下的调用方对象（别名保留旧调用点写法） */
    const DebugCallbacks *cb     = m_cb;

    DWORD DBGCode = DBG_CONTINUE;
    m_last_error  = NULL;   /* 只有本函数会写；读它的人只在返回 -1 时读 */
    if (cb->on_any_debug_event)
    {
        core::DebugEvent ev;
        ev.pid  = DBGEvent.dwProcessId;
        ev.tid  = DBGEvent.dwThreadId;
        ev.code = DBGEvent.dwDebugEventCode;
        cb->on_any_debug_event(cb->context, ev);
    }
    if (params->use_scylla_hide)
        scylla_hide_on_debug();
    switch (DBGEvent.dwDebugEventCode)
    {
    case EXCEPTION_DEBUG_EVENT:
        if (cb->on_exception)
        {
            core::ExceptionEvent ev;
            ev.pid  = DBGEvent.dwProcessId;
            ev.tid  = DBGEvent.dwThreadId;
            ev.code = DBGEvent.u.Exception.ExceptionRecord.ExceptionCode;
            ev.addr = (size_t)DBGEvent.u.Exception.ExceptionRecord.ExceptionAddress;
            cb->on_exception(cb->context, ev);
        }
        DBGCode = DBG_EXCEPTION_NOT_HANDLED;
        switch (DBGEvent.u.Exception.ExceptionRecord.ExceptionCode)
        {
        case EXCEPTION_BREAKPOINT: // 遇到断点。
            if (cb->on_breakpoint)
            {
                core::BreakpointEvent ev;
                ev.pid  = DBGEvent.dwProcessId;
                ev.tid  = DBGEvent.dwThreadId;
                ev.addr = (size_t)DBGEvent.u.Exception.ExceptionRecord.ExceptionAddress;
                cb->on_breakpoint(cb->context, ev);
            }
            if (params->use_scylla_hide)
                scylla_hide_on_breakpoint();
            if (m_first_bp)
            {
                DBGCode = DBG_CONTINUE;
                if (cb->on_first_breakpoint)
                {
                    core::FirstBreakpointEvent ev;
                    ev.pid = DBGEvent.dwProcessId;
                    ev.tid = DBGEvent.dwThreadId;
                    cb->on_first_breakpoint(cb->context, ev);
                }
                m_first_bp = false;
            }
            else
            {
                size_t         ea     = (size_t)DBGEvent.u.Exception.ExceptionRecord.ExceptionAddress;
                PEInfo *peinfo = get_peinfo_by_ea(ea);
                if (peinfo)
                {
                    LPTSTR                  modname = peinfo->Path + peinfo->name_offset;
                    DWORD                   rva     = (DWORD)(ea - peinfo->base);
                    BreakpointInfo *tmp = breakpoints().head();
                    while (tmp)
                    {
                        BreakpointInfo *bp_next = tmp->next; /* 先留 next（remove 会置空） */
                        if (!tmp->activate)
                        {
                            tmp = bp_next;
                            continue;
                        }
                        if (rva != tmp->rva || _tcsicmp(modname, tmp->modname))
                        {
                            tmp = bp_next;
                            continue;
                        }
                        if (!tmp->has_orignal_byte)
                        {
                            /* 旧实现在这里直接 lua_pushstring + lua_error（长跳、跳过清理）。
                             * 现改为「走完清理尾部 + 返回错误码」，由绑定层在 run 返回后
                             * lua_error —— Lua 可见行为一致，但不再长跳穿 C++ 帧（D18）。*/
                            run_shutdown(params);
                            m_last_error = "tmp->has_orignal_byte is zero!";
                            return -1;
                        }
                        disable_breakpoint(ea, modname, rva);
                        DBGCode         = DBG_CONTINUE;
                        HANDLE  hThread = get_thread_by_tid(DBGEvent.dwThreadId);
                        CONTEXT context;
                        context.ContextFlags = CONTEXT_CONTROL;
                        GetThreadContext(hThread, &context);
                        if (!tmp->is_single)
                            context.EFlags |= 0x100;
#if defined(_WIN64)
                        context.Rip--;
#else
                        context.Eip--;
#endif
                        SetThreadContext(hThread, &context);
                        callback_ref_t bp_callback = tmp->callback;
                        bool           to_unref_cbk = false;
                        if (tmp->is_single)
                        {
                            /* P6.2：摘除 + 释放一次完成（旧写法 `*pp = (*pp)->next; free_(tmp);`） */
                            breakpoints().remove_and_free(tmp);
                            to_unref_cbk = true;
                        }
                        else
                        {
                            BYTE original_byte;
                            if (read(ea, &original_byte, 1))
                            {
                                if (original_byte == 0x9C)
                                    m_push_fb = true;

                                // 保存重置断点信息
                                m_reset_bpx.modname = modname;
                                m_reset_bpx.rva     = rva;
                                m_reset_bpx.ea      = ea;
                            }
                        }
                        if (bp_callback.valid)
                        {
                            core::BreakpointCallbackEvent ev;
                            ev.callback    = bp_callback;
                            ev.pid         = DBGEvent.dwProcessId;
                            ev.tid         = DBGEvent.dwThreadId;
                            ev.ea          = ea;
                            ev.rva         = rva;
                            ev.modname     = modname;
                            ev.single_shot = to_unref_cbk;
                            cb->on_breakpoint_callback(cb->context, ev);
                        }
                        tmp = bp_next; /* 处理完一个节点后前进（旧实现靠 `activate` 被置 0 来落地下一轮） */
                    }
                }
            }
            break;
        case EXCEPTION_SINGLE_STEP: // 跟踪陷阱或其他单指令机制指示已执行一个指令。
            if (cb->on_single_step)
            {
                core::StepEvent ev;
                ev.pid  = DBGEvent.dwProcessId;
                ev.tid  = DBGEvent.dwThreadId;
                ev.addr = (size_t)DBGEvent.u.Exception.ExceptionRecord.ExceptionAddress;
                cb->on_single_step(cb->context, ev);
            }
            if (m_reset_bpx.modname || m_reset_hw_bp > 0)
            {
                DBGCode = DBG_CONTINUE;
                if (m_push_fb)
                {
                    m_push_fb = false;
                    // 处理pushf指令特殊情况
                    HANDLE hThread = get_thread_by_tid(DBGEvent.dwThreadId);
                    if (hThread)
                    {
                        CONTEXT context;
                        context.ContextFlags = CONTEXT_CONTROL;
                        if (GetThreadContext(hThread, &context))
                        {
#if defined(_WIN64)
                            size_t csp = context.Rsp;
#else
                            size_t csp = context.Esp;
#endif
                            size_t data;
                            if (read(csp, &data, sizeof data))
                            {
                                data &= ~0x100;
                                write(csp, &data, sizeof data, false);
                            }
                        }
                    }
                }

                if (m_reset_bpx.modname)
                {
                    // 验证当前地址不是重置断点地址
                    size_t current_ea = (size_t)DBGEvent.u.Exception.ExceptionRecord.ExceptionAddress;

                    assert(m_reset_bpx.ea != current_ea);
                    // 重新启用断点
                    enable_breakpoint(m_reset_bpx.ea, m_reset_bpx.modname, m_reset_bpx.rva);

                    // 清理m_reset_bpx
                    m_reset_bpx.modname = NULL;
                    m_reset_bpx.rva     = 0;
                    m_reset_bpx.ea      = 0;
                }

                if (m_reset_hw_bp > 0)
                {
                    for (int i = 0; i < 4; i++)
                    {
                        if ((1 << i) & m_reset_hw_bp)
                            enable_hardware_breakpoint(i);
                    }
                    m_reset_hw_bp = 0;
                }
            }
            else
            {
                HANDLE hThread = get_thread_by_tid(DBGEvent.dwThreadId);
                if (hThread)
                {
                    CONTEXT context;
                    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_DEBUG_REGISTERS;
                    if (GetThreadContext(hThread, &context))
                    {
                        for (int i = 0; i < 4; i++)
                        {
                            if ((&context.Dr0)[i] == (ULONG_PTR)DBGEvent.u.Exception.ExceptionRecord.ExceptionAddress || ((BYTE)context.Dr6 & (1 << i)))
                            {
                                if (!hw_breakpoints()[i].activate)
                                    DBGCode = DBG_EXCEPTION_NOT_HANDLED;
                                else
                                {
                                    DBGCode = DBG_CONTINUE;
                                    {
                                        context.EFlags |= 0x100;
                                        SetThreadContext(hThread, &context);
                                    }
                                    callback_ref_t hw_callback = hw_breakpoints()[i].callback;
                                    if (hw_callback.valid)
                                    {
                                        core::HwBreakpointCallbackEvent ev;
                                        ev.callback  = hw_callback;
                                        ev.ea        = (size_t)DBGEvent.u.Exception.ExceptionRecord.ExceptionAddress;
                                        ev.pid       = DBGEvent.dwProcessId;
                                        ev.tid       = DBGEvent.dwThreadId;
                                        cb->on_hw_breakpoint_callback(cb->context, ev);
                                    }
                                    m_reset_hw_bp |= 1 << i;
                                    disable_hardware_breakpoint(i);
                                }
                            }
                        }
                    }
                }
            }
            break;
        default:
            if (cb->on_user_exception)
            {
                core::UserExceptionEvent ev;
                ev.pid          = DBGEvent.dwProcessId;
                ev.tid          = DBGEvent.dwThreadId;
                ev.first_chance = !!DBGEvent.u.Exception.dwFirstChance;
                ev.record       = &DBGEvent.u.Exception.ExceptionRecord;
                cb->on_user_exception(cb->context, ev);
            }
            DBGCode = DBG_EXCEPTION_NOT_HANDLED;
            break;
        }
        break;
    case CREATE_THREAD_DEBUG_EVENT:
        if (cb->on_create_thread)
        {
            core::ThreadEvent ev;
            ev.pid  = DBGEvent.dwProcessId;
            ev.tid  = DBGEvent.dwThreadId;
            ev.addr = (size_t)DBGEvent.u.CreateThread.lpStartAddress;
            cb->on_create_thread(cb->context, ev);
        }
        {
            /* P6.2：直接由链表分配（`OwnedIntrusiveList::create_front`）——与旧代码
             * `AllocX + next = head + head = info` 完全等价，只是把所有权交给链表。 */
            ThreadInfo *info       = threads().create_front();
            info->dwThreadId        = DBGEvent.dwThreadId;
            info->hThread           = DBGEvent.u.CreateThread.hThread;
            info->ProcessId         = DBGEvent.dwProcessId;
            info->lpStartAddress    = (size_t)DBGEvent.u.CreateThread.lpStartAddress;
            break;
        }
    case CREATE_PROCESS_DEBUG_EVENT:
        if (!m_first_create_process)
        {
            m_first_create_process = true;
            /* P6.1：映像文件句柄归调试器所有（MSDN）——旧代码只把它存进 `dbgfilehandle` 当标志、
             * **从未关闭** → 现在用完立即关（RAII 到本作用域末）；行为变更（修复句柄泄漏）。 */
            unique_handle image_file(DBGEvent.u.CreateProcessInfo.hFile);
            if (params->pid && params->pid == DBGEvent.dwProcessId)
            {
                proc().hProcess   = DBGEvent.u.CreateProcessInfo.hProcess;
                proc().hThread    = DBGEvent.u.CreateProcessInfo.hThread;
                proc().dwThreadId = DBGEvent.dwThreadId;
            }
        }
        {
            TCHAR image_name[MAX_FILEPATH];
#ifdef _UNICODE
            DWORD image_name_len =
                GetMappedFileNameW(DBGEvent.u.CreateProcessInfo.hProcess, DBGEvent.u.CreateProcessInfo.lpBaseOfImage, image_name, _countof(image_name));
#else
            DWORD image_name_len = LjGetMappedFileNameA(DBGEvent.dwProcessId, DBGEvent.u.CreateProcessInfo.lpBaseOfImage, image_name, _countof(image_name));
#endif
            LPCTSTR translated_name = TranslateNativeName(image_name);
            image_name_len -= (DWORD)(translated_name - image_name);
            /* P6.2：变长节点由链表一次分配（`sizeof(PEInfo) + n*sizeof(TCHAR)`，与旧
             * `alloc_(info_size)` 同一算法）；`create_front` 返回的节点**已整块清零**（含 next 已接好）。 */
            PEInfo *info = peinfos().create_front(image_name_len * sizeof(TCHAR));
            info->dwProcessId   = DBGEvent.dwProcessId;
            info->base          = (size_t)DBGEvent.u.CreateProcessInfo.lpBaseOfImage;
            info->start_address = (size_t)DBGEvent.u.CreateProcessInfo.lpStartAddress;
            info->name_offset   = PathFindFileName(translated_name) - translated_name;
            /* ❗按 TCHAR 的**宽**度拷：legacy（ANSI）变体里 TCHAR 是 1 字节，用 `__movsw`
             * 会按 2 字节一个单元拷 `image_name_len` 个 → 越界写掉半个缓冲（分配只有
             * `image_name_len * sizeof(TCHAR)` 字节）→ 堆损坏，表现为随机 AV。 */
#ifdef _UNICODE
            __movsw((LPWORD)info->Path, (const WORD *)translated_name, image_name_len);
#else
            __movsb((PBYTE)info->Path, (const BYTE *)translated_name, image_name_len);
#endif
            info->Path[image_name_len] = 0;
            if (!calc_crc32(info->Path, &info->crc32))
                cont() = false;
            update_breakpoints(info);
            if (cb->on_create_process)
            {
                core::ProcessEvent ev;
                ev.pid  = DBGEvent.dwProcessId;
                ev.tid  = DBGEvent.dwThreadId;
                ev.addr = (size_t)DBGEvent.u.CreateProcessInfo.lpStartAddress;
                ev.base = (size_t)DBGEvent.u.CreateProcessInfo.lpBaseOfImage;
                ev.path = info->Path;
                cb->on_create_process(cb->context, ev);
            }
        }
        if (params->use_scylla_hide)
            scylla_hide_on_create_process(DBGEvent.dwProcessId, DBGEvent.u.CreateProcessInfo.lpStartAddress);
        {
            ThreadInfo *info       = threads().create_front();
            info->dwThreadId        = DBGEvent.dwThreadId;
            info->hThread           = DBGEvent.u.CreateProcessInfo.hThread;
            info->ProcessId         = DBGEvent.dwProcessId;
        }
        break;
    case EXIT_THREAD_DEBUG_EVENT:
        if (cb->on_exit_thread)
        {
            core::ExitEvent ev;
            ev.pid  = DBGEvent.dwProcessId;
            ev.tid  = DBGEvent.dwThreadId;
            ev.code = DBGEvent.u.ExitThread.dwExitCode;
            cb->on_exit_thread(cb->context, ev);
        }
        {
            /* P6.2：按 tid 找节点 → 关句柄（P6.1 的修复，D29）→ 摘除并释放 */
            for (ThreadInfo *tmp = threads().head(); tmp; tmp = tmp->next)
            {
                if (tmp->dwThreadId != DBGEvent.dwThreadId)
                    continue;
                if (tmp->hThread)
                    CloseHandle(tmp->hThread);
                threads().remove_and_free(tmp);
                break;
            }
            break;
        }
    case EXIT_PROCESS_DEBUG_EVENT:
        if (cb->on_exit_process)
        {
            core::ExitEvent ev;
            ev.pid  = DBGEvent.dwProcessId;
            ev.tid  = DBGEvent.dwThreadId;
            ev.code = DBGEvent.u.ExitProcess.dwExitCode;
            cb->on_exit_process(cb->context, ev);
        }
        {
            if (DBGEvent.dwProcessId == proc().dwProcessId)
                cont() = false;
            DBGCode            = DBG_CONTINUE;
            for (PEInfo *tmp = peinfos().head(); tmp; )
            {
                PEInfo *next = tmp->next; /* 先留 next（remove 会把它置空） */
                if (tmp->dwProcessId == DBGEvent.dwProcessId)
                {
                    del_breakpoints_by_modname_without_update_memory(cb, tmp->Path + tmp->name_offset);
                    peinfos().remove_and_free(tmp);
                }
                tmp = next;
            }
            break;
        }
    case LOAD_DLL_DEBUG_EVENT:
    {
        /* P6.1：本事件带的 `hFile` 归调试器所有、用完就该关（旧实现从不关 → 每加载一个 DLL 泄漏一个句柄）。
         * RAII 到本 case 结束；日志文案里仍打印原表达式以保持文本一致。 */
        unique_handle load_dll_file(DBGEvent.u.LoadDll.hFile);
        TCHAR         image_name[MAX_FILEPATH];
#if !defined(_UNICODE)
        DWORD image_name_len = LjGetMappedFileNameA(proc().dwProcessId, DBGEvent.u.LoadDll.lpBaseOfDll, image_name, _countof(image_name));
        if (!image_name_len)
            E("LjGetMappedFileNameA(0x%zX)", DBGEvent.u.LoadDll.lpBaseOfDll);
#elif defined(_WIN64)
        DWORD image_name_len = GetFinalPathNameByHandleW(DBGEvent.u.LoadDll.hFile, image_name, _countof(image_name), VOLUME_NAME_NT);
        if (!image_name_len)
            E("GetFinalPathNameByHandleW(0x%zX)", DBGEvent.u.LoadDll.lpBaseOfDll);
#else
        DWORD image_name_len = GetMappedFileNameW(proc().hProcess, DBGEvent.u.LoadDll.lpBaseOfDll, image_name, _countof(image_name));
        if (!image_name_len)
            E("GetMappedFileNameW(0x%zX)", DBGEvent.u.LoadDll.lpBaseOfDll);
#endif
        PEInfo *info = NULL;
        if (image_name_len)
        {
            LPCTSTR translated_name = TranslateNativeName(image_name);
            image_name_len -= (DWORD)(translated_name - image_name);
            /* P6.2：变长节点由链表一次分配（与旧 `alloc_(sizeof(PEInfo) + n*sizeof(TCHAR))` 同算法） */
            info         = peinfos().create_front(image_name_len * sizeof(TCHAR));
            info->base        = (size_t)DBGEvent.u.LoadDll.lpBaseOfDll;
            info->name_offset = PathFindFileName(translated_name) - translated_name;
            /* ❗同 CREATE_PROCESS 那处：legacy（ANSI）的 TCHAR 是 1 字节，`__movsw` 会
             * 按 2 字节拷 → 溢出分配的 `image_name_len` 字节 → 堆损坏（随机 AV）。 */
#ifdef _UNICODE
            __movsw((LPWORD)info->Path, (const WORD *)translated_name, image_name_len);
#else
            __movsb((PBYTE)info->Path, (const BYTE *)translated_name, image_name_len);
#endif
            info->Path[image_name_len] = 0;
            if (!calc_crc32(info->Path, &info->crc32))
                cont() = false;
            update_breakpoints(info);
        }
        if (cb->on_load_dll)
        {
            core::DllEvent ev;
            ev.pid  = DBGEvent.dwProcessId;
            ev.tid  = DBGEvent.dwThreadId;
            ev.base = (size_t)DBGEvent.u.LoadDll.lpBaseOfDll;
            ev.path = info ? info->Path : NULL;
            cb->on_load_dll(cb->context, ev);
        }
        if (params->use_scylla_hide)
            scylla_hide_on_create_dll(DBGEvent.u.LoadDll.lpBaseOfDll);
        break;
    }
    case UNLOAD_DLL_DEBUG_EVENT:
        if (cb->on_unload_dll)
        {
            core::DllEvent ev;
            ev.pid  = DBGEvent.dwProcessId;
            ev.tid  = DBGEvent.dwThreadId;
            ev.base = (size_t)DBGEvent.u.UnloadDll.lpBaseOfDll;
            ev.path = NULL;
            cb->on_unload_dll(cb->context, ev);
        }
        {
            for (PEInfo *tmp = peinfos().head(); tmp; )
            {
                PEInfo *next = tmp->next;
                if (tmp->base == (size_t)DBGEvent.u.UnloadDll.lpBaseOfDll)
                {
                    del_breakpoints_by_modname_without_update_memory(cb, tmp->Path + tmp->name_offset);
                    peinfos().remove_and_free(tmp);
                }
                tmp = next;
            }
            break;
        }
    case OUTPUT_DEBUG_STRING_EVENT:
    {
        // ODS 事件的 ContinueDebugEvent 状态**必须是 DBG_CONTINUE**（MSDN）。
        // 旧代码用 DBG_EXCEPTION_NOT_HANDLED → 实测（本机 Win11 + 六产物）会导致
        // 同一个 ODS 事件被重投递，回调/回显各触发两次（缺陷 D11，已修）。
        DBGCode              = DBG_CONTINUE;
        DWORD       ods_size = DBGEvent.u.DebugString.nDebugStringLength;
        bool        ods_uni  = DBGEvent.u.DebugString.fUnicode != 0;
        const void *ods_data = NULL;
        void       *p        = NULL;
        if (DBGEvent.u.DebugString.lpDebugStringData && DBGEvent.u.DebugString.nDebugStringLength)
        {
            p = alloc_(DBGEvent.u.DebugString.nDebugStringLength);
            if (read((size_t)DBGEvent.u.DebugString.lpDebugStringData, p, DBGEvent.u.DebugString.nDebugStringLength))
                ods_data = p;
        }
        /* 控制台回显（不属于 Lua，留在 core）：与旧实现同序 —— 先回显、后回调
         * ❗`nDebugStringLength` **含**结尾的 NUL（MSDN："including the terminating null"）
         *   ⇒ 直接抳长度会把 0x00 一起写进输出（`--json` 的信封里表现为 `stdout` 里
         *   混进 `\u0000`，那一行也不再是合法 JSON 的单行文本）。这里去掉结尾的 NUL，
         *   并把同一份长度交给 Lua 回调（否则脚本里的 `ev.str` 带上 `\0` 又会污染 stdout）。 */
        if (ods_data)
        {
            if (!ods_uni)
            {
                if (ods_size > 0 && ((const char *)ods_data)[ods_size - 1] == '\0')
                    ods_size--;
                PrintN((LPCSTR)ods_data, ods_size);
            }
#ifdef _UNICODE
            else
            {
                if (ods_size >= sizeof(WCHAR) && ((const WCHAR *)ods_data)[ods_size / sizeof(WCHAR) - 1] == L'\0')
                    ods_size -= sizeof(WCHAR);
                PrintN((LPCWSTR)ods_data, ods_size / sizeof(WCHAR));
            }
#endif
        }
        /* 旧行为：只要登记了回调就**总是**调用（读取失败时表里没有 str 字段） */
        if (cb->on_output_debug_string)
        {
            core::OutputDebugStringEvent ev;
            ev.pid     = DBGEvent.dwProcessId;
            ev.tid     = DBGEvent.dwThreadId;
            ev.data    = ods_data;
            ev.size    = ods_size;
            ev.unicode = ods_uni;
            cb->on_output_debug_string(cb->context, ev);
        }
        if (p)
            free_(p);
        break;
    }
    case RIP_EVENT:
        if (cb->on_rip)
        {
            core::DebugEvent ev;
            ev.pid  = DBGEvent.dwProcessId;
            ev.tid  = DBGEvent.dwThreadId;
            ev.code = DBGEvent.dwDebugEventCode;
            cb->on_rip(cb->context, ev);
        }
        break;
    }

    /* `autocont = false` ⇒ ❗不 continue：事件停在未应答状态（`DBG_EVENT_UNHANDLED` 就是为此存在），
     * 由调用方显式 `cont(pid, tid, code)`。 */
    m_last_code = DBGCode;   /* T7.3（§4.4）：存下这一轮的默认 code，绑定层读它（`pump.default-code-parity`） */
    if (autocont)
    {
        if (!cont(DBGEvent.dwProcessId, DBGEvent.dwThreadId, DBGCode))
        {
            E("ContinueDebugEvent");
            m_cont = false; /* 旧的 `break` */
        }
    }
    return (int)DBGEvent.dwDebugEventCode;
}

/* pump = `wait_one` + `dispatch`（T7.1 的「一次 WaitForDebugEvent + 派发」原样保留，C2a 只是拆开）。
 * 返回调试事件码（0 = 没等到事件）；-1 = 引擎内部错误（`*err` 已置，且收尾已在这条路径里跑过）。 */
int DebuggerSession::pump(DWORD timeout_ms, bool autocont, const char **err)
{
    DEBUG_EVENT ev;
    int         r = wait_one(timeout_ms, &ev, err);
    if (r <= 0)
        return r;                   /* 0 = 没等到事件 / -1 = 等待期引擎错误 */
    r = dispatch(ev, autocont);
    if (r < 0 && err)
        *err = m_last_error;
    return r;
}

/* cont：ContinueDebugEvent（T7.1）—— `code` 显式（含「原样交给程序」）。 */
bool DebuggerSession::cont(DWORD pid, DWORD tid, DWORD code)
{
    return ContinueDebugEvent(pid, tid, code) != 0;
}

/* end：收尾（原 `run` 的尾部）。
 * ❗T7.3：`to_detach()` 为真时先做**真 detach**（D47）—— 恢复断点原字节 + `DebugActiveProcessStop`，
 *   目标**继续活着**且 pid 不变（下次可再 `Attach`）。这是 `dbg.detach()` 对「附加来的目标」的口径；
 *   `Run`/`Attach` 的旧路径（`to_detach` 为假）行为一字不变。
 * 返回值：0 = 正常收尾（该终止就终止）；**1 = 要求 detach 但系统没有 `DebugActiveProcessStop`
 *   （Win95/98）⇒ 已降级成 terminate**（D47 的「能力上报」）；-1 保留给以后的错误。 */
int DebuggerSession::end(const char **err)
{
    (void)err; /* 目前不产生错误；与 begin / pump 保持同一签名 */
    int ret = 0;
    if (to_detach())
    {
        term() = false;          /* 不杀：detach 的语义就是「放手让目标继续跑」 */
        detach_process(m_cb);    /* 恢复所有断点的原字节 + 通知销毁（令牌） */
        if (m_debug_active_process_stop)
            m_debug_active_process_stop(proc().dwProcessId);
        else
        {
            term() = true;       /* 9x：没有该导出 ⇒ 与 D47 一样降级成 kill */
            ret    = 1;
        }
        to_detach() = false;     /* 只消费一次（否则下一次 end 会再 detach 一遍） */
    }
    run_shutdown(m_params);
    return ret;
}

/* pump_now() = pump(0, false)：REPL 与回调里唯一能用的形态（不阻塞、不 continue） */
int DebuggerSession::pump_now(const char **err)
{
    return pump(0, false, err);
}

/* run（`lua_Run` / `lua_Attach` 仍调它）= begin + while + pump(autocont = true) + end 的封装。
 * 返回 0 = 正常结束；-1 = 引擎内部错误（*err 指向静态字符串）。
 * ❗ 错误**不用长跳**表达（D18）：内部错误正常返回，由绑定层在 run 返回后 lua_error。 */
int DebuggerSession::run(const core::RunParams *params, const core::DebugCallbacks *cb, const char **err)
{
    int r = begin(params, cb, err);
    if (r == -2)
        return 0; /* 1:1：附加失败时旧实现直接返回 0（不跑收尾） */
    if (r != 0)
        return r;

    while (cont())
    {
        r = pump(100, true, err);
        if (r < 0)
            return r; /* pump 的内部错误路径已自行跑过收尾（1:1） */
    }
    return end(err);
}
