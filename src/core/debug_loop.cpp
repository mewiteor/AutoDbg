/* =============================================================================
 * src/core/debug_loop.cpp —— 调试事件主循环（P3.B 模块 9，逐字从 bindings.cpp 迁入）
 *
 * 内容：`run`（WaitForDebugEvent 主循环 + 事件分发 + 全部回调调用）、`run_shutdown`、
 *       以及 ScyllaHide 的条件引入（98 构建用空实现）。
 * 跨模块改动（仅这些）：
 *   · `run` 去掉 `static`（`lua_Run` / `lua_Attach` 调用）；
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

/* 返回 0 = 正常结束；-1 = 引擎内部错误（*err 指向静态字符串）。
 * ❗ 错误**不用长跳**表达：旧实现在此直接 lua_error（会跨 C++ 帧 longjmp，见 D18），
 * 现改为正常返回，由绑定层在 run 返回后 lua_error（Lua 可见行为一致）。 */
int DebuggerSession::run(const core::RunParams *params, const core::DebugCallbacks *cb, const char **err)
{
    if (params->use_scylla_hide)
        scylla_hide_setup();

    if (params->pid)
    {
        if (!DebugActiveProcess(params->pid))
        {
            E("DebugActiveProcess");
            return 0; /* 1:1：旧实现就是直接返回（不报 Lua 错误，脚本继续） */
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
    cont()               = true;
    term()               = true;
    to_detach()          = false;
    BOOL   FirstBPX      = TRUE;
    BOOL   pushfbpx      = FALSE;
    bool   first_create_process = false; /* 旧 `dbgfilehandle` 实际只当「首个 CREATE_PROCESS」标志用 */

    struct
    {
        LPCTSTR modname;
        DWORD   rva;
        size_t  ea;
    } reset_bpx = {NULL, 0, 0}; /* ❗D32 修复：必须显式初始化 —— 否则 `reset_bpx.modname` 是栈垃圾，
                                 * 会让「首个单步事件」误入下面的 re-arm 分支：① 硬件断点的首个命中会被
                                 * 静默丢掉；② 还会拿垃圾指针去调 `enable_breakpoint()`（debug 构建下先撞 assert）。
                                 * 这就是「武装后的首次访问必定漏掉」的根因（垃圾值随栈内容变化 → 早先还表现为不稳定）。 */

    int reset_hwbpx = 0;

    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    typedef BOOL(__stdcall * DebugActiveProcessStopT)(DWORD dwProcessId);
    DebugActiveProcessStopT DebugActiveProcessStop_ = (DebugActiveProcessStopT)GetProcAddress(hKernel32, "DebugActiveProcessStop");

    while (cont())
    {
        DEBUG_EVENT DBGEvent;
        DWORD       DBGCode  = DBG_CONTINUE;
        bool        detached = false;
        if (!WaitForDebugEvent(&DBGEvent, 100))
        {
            if (to_detach())
            {
                term() = false;
                detach_process(cb);
                detached = true;
            }
            else if (cb->on_wait_timeout)
            {
                /* 引擎只执行结论；「能不能分离而不终止」由引擎告知（DebugActiveProcessStop 可用性） */
                switch (cb->on_wait_timeout(cb->context, !!DebugActiveProcessStop_))
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
                if (DebugActiveProcessStop_)
                    DebugActiveProcessStop_(proc().dwProcessId);
                break;
            }
            continue;
        }
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
                if (FirstBPX)
                {
                    DBGCode = DBG_CONTINUE;
                    if (cb->on_first_breakpoint)
                    {
                        core::FirstBreakpointEvent ev;
                        ev.pid = DBGEvent.dwProcessId;
                        ev.tid = DBGEvent.dwThreadId;
                        cb->on_first_breakpoint(cb->context, ev);
                    }
                    FirstBPX = FALSE;
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
                                *err = "tmp->has_orignal_byte is zero!";
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
                                        pushfbpx = TRUE;

                                    // 保存重置断点信息
                                    reset_bpx.modname = modname;
                                    reset_bpx.rva     = rva;
                                    reset_bpx.ea      = ea;
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
                if (reset_bpx.modname || reset_hwbpx > 0)
                {
                    DBGCode = DBG_CONTINUE;
                    if (pushfbpx)
                    {
                        pushfbpx = FALSE;
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

                    if (reset_bpx.modname)
                    {
                        // 验证当前地址不是重置断点地址
                        size_t current_ea = (size_t)DBGEvent.u.Exception.ExceptionRecord.ExceptionAddress;

                        assert(reset_bpx.ea != current_ea);
                        // 重新启用断点
                        enable_breakpoint(reset_bpx.ea, reset_bpx.modname, reset_bpx.rva);

                        // 清理reset_bpx
                        reset_bpx.modname = NULL;
                        reset_bpx.rva     = 0;
                        reset_bpx.ea      = 0;
                    }

                    if (reset_hwbpx > 0)
                    {
                        for (int i = 0; i < 4; i++)
                        {
                            if ((1 << i) & reset_hwbpx)
                                enable_hardware_breakpoint(i);
                        }
                        reset_hwbpx = 0;
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
                                        reset_hwbpx |= 1 << i;
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
            if (!first_create_process)
            {
                first_create_process = true;
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
                __movsw((LPWORD)info->Path, (const WORD *)translated_name, image_name_len);
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
                E("GetFinalPathNameByHandleW(0x%zX)", DBGEvent.u.LoadDll.hFile);
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
                __movsw((LPWORD)info->Path, (const WORD *)translated_name, image_name_len);
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
        if (!ContinueDebugEvent(DBGEvent.dwProcessId, DBGEvent.dwThreadId, DBGCode))
        {
            E("ContinueDebugEvent");
            break;
        }
    }
    run_shutdown(params);
    return 0;
}
