/* =============================================================================
 * src/core/process.cpp —— 进程 / 线程控制（P3.B 模块 7，逐字从 bindings.cpp 迁入）
 *
 * 内容：get_thread_by_tid / detach_process / find_pid_by_mouse（+ 内部辅助 FlashWin），
 *   以及 T7 切片 C2b 的进程树（`add_pid` / `owns_pid` / `pid_at` / `handle_for_pid` /
 *   `close_pid_handle(s)`，plan_t7.md §10 第 1/3 条）。
 *
 * 跨模块改动（仅这些，其余逐字节不变）：
 *   · get_thread_by_tid / detach_process 去掉 `static`（绑定层调用）；
 *   · find_pid_by_mouse：`lua_State *L` → `const char **err` —— 「未选中任何窗口」
 *     改为把文案交还调用方，由绑定层 `lua_error`（Lua 可见行为一致）；其余失败路径
 *     1:1 静默返回 0；
 *   · FlashWin 保持 `static`（只有 find_pid_by_mouse 用它）；
 *   · 两处 `free(...)` → `free_(...)`（同名桥接：`core/util.hpp` 的 `free_` 就是
 *     `common::release` → msvcrt `free`，且多一个 NULL 判断）；引擎侧不再直接依赖
 *     CRT 的 `free` 声明（旧文件是从 `<stdlib.h>` 传递性拿到的）。
 *   · C2b 追加进程树：pid 集合（根 + 调试期间出现的 CREATE_PROCESS）与 pid→HANDLE 缓存
 *     （非根进程走 `OpenProcess`；`PROCESS_QUERY_INFORMATION` 在 9x 上不认，去掉它重试）。
 * ❗ 控制台输入策略（CheckConsoleInput）**不在这里** —— 见 Q27 / process.hpp 的说明。
 * 依赖：memory（write）、pe（get_base）、engine（g_breakpoint_list）、util（打印）。
 * ========================================================================== */
#include "process.hpp"
#include "core.hpp" /* P6.4：引擎函数是会话成员（本 TU 多数定义即成员，直接用自身状态） */
#include "types.hpp"
#include "memory.hpp"
#include "pe.hpp"
#include "util.hpp"

#include <intrin.h> /* ZeroMem 宏（__stosb）；与 breakpoints.cpp 同一做法 */

/* 类型统一（删 legacy_types.h）：这些名字现在只有 types.hpp 里的 `autodbg::core::` 那套 */
using autodbg::core::BreakpointInfo;
using autodbg::core::ThreadInfo;

namespace core = autodbg::core;       /* 与其它 core 模块一致：显式限定，避免与 C 头同名类型二义 */
using autodbg::core::DebuggerSession; /* P6.4：引擎函数已是它的成员 */

/* 迁移期便利：本文件是第一个用到 core 打印的 core 模块（旧调用点不带限定名）。
 * 仍然刻意**不用** `using namespace autodbg::core`：选择性 using 更清楚，
 * 也不会把整个命名空间（含大量调试器类型/函数）一次性拉进全局作用域。 */
using autodbg::core::LjGetConsoleWindow;
using autodbg::core::LjGetRootWindow; /* Win95 兼容：GetAncestor 的动态包装 */
using autodbg::core::LjFlashWindow;   /* Win95 兼容：FlashWindowEx 的动态包装 */
using autodbg::core::PrintF;
using autodbg::core::PrintN;
using autodbg::core::PrintS;

HANDLE DebuggerSession::get_thread_by_tid(DWORD tid)
{
    for (ThreadInfo *info = threads().head(); info; info = info->next)
    {
        if (info->dwThreadId == tid)
            return info->hThread;
    }
    return NULL;
}

void DebuggerSession::detach_process(const core::DebugCallbacks *cb)
{
    /* P6.2：逐个「恢复原字节 + 通知销毁 + 释放节点」（顺序与旧实现一致） */
    for (BreakpointInfo *bp = breakpoints().head(); bp;)
    {
        BreakpointInfo *next = bp->next; /* 先留 next（remove 会置空） */
        if (bp->activate && bp->has_orignal_byte)
        {
            size_t base = get_base(bp->modname);
            if (base)
                write(base + bp->rva, &bp->orignal_byte, 1, true);
        }
        if (cb && cb->on_breakpoint_destroyed && bp->callback.valid)
            cb->on_breakpoint_destroyed(cb->context, bp->callback);
        breakpoints().remove_and_free(bp);
        bp = next;
    }
}

static void FlashWin(HWND hwnd)
{
    /* Win95 兼容：`FlashWindowEx` 不在 Win95 的 user32 里 → 走 util.cpp 的动态解析 + 回落 */
    LjFlashWindow(hwnd);
}

/* ========================= 进程树（T7 切片 C2b，plan_t7.md §10） =========================
 * 会话的键是**一棵树**：根 pid + 调试期间出现的全部 `CREATE_PROCESS` 的 pid（一棵树只有一个调试器
 * ⇒ 那个 pid 一定属于本会话）。这里只放**集合与句柄**：
 *   · 归属维护在两个事件分支（`debug_loop.cpp` 的 CREATE_PROCESS / EXIT_PROCESS）；
 *   · 内存 API 的 pid 重载在 `memory.cpp`（真正用句柄的地方）；
 *   · 容器是两个定长数组（零 CRT 下不为它引入容器；单棵树的进程数是个位数，见 core.hpp）。 */

void DebuggerSession::add_pid(DWORD pid) noexcept
{
    if (!pid) return;
    for (int i = 0; i < m_pid_count; i++)
        if (m_pids[i] == pid) return;
    if (m_pid_count >= kMaxPids) return;   /* 满了：多出来的 pid 不归本会话（`handle_for_pid` 会拒） */
    m_pids[m_pid_count++] = pid;
}

bool DebuggerSession::owns_pid(DWORD pid) const noexcept
{
    if (!pid) return false;
    for (int i = 0; i < m_pid_count; i++)
        if (m_pids[i] == pid) return true;
    return false;
}

DWORD DebuggerSession::pid_at(int index) const noexcept
{
    if (index < 0 || index >= m_pid_count) return 0;
    return m_pids[index];
}

/* 内存/断点补丁该用哪个进程句柄（plan_t7.md §10 第 3 条：旧实现一律 `proc().hProcess` ⇒
 * 子进程的断点补丁会打到根进程上）。`pid == 0` 与根 pid 都返回 `proc().hProcess` ——
 * 那个句柄是 `DEBUG_PROCESS` 换来的（权限最全），由 `run_shutdown` 关，不归这里的缓存。 */
HANDLE DebuggerSession::handle_for_pid(DWORD pid)
{
    if (!pid || pid == proc().dwProcessId) return proc().hProcess;

    if (!owns_pid(pid)) return NULL;   /* 不是本会话树里的进程：不替调用方去开别人的进程 */

    for (int i = 0; i < kMaxPids; i++)
        if (m_pid_handles[i].pid == pid) return m_pid_handles[i].handle;

    /* 非根进程：`OpenProcess` 拿一份自己拥有的句柄并**缓存**（进程退出时 `close_pid_handle`、
     * 收尾/析构时 `close_pid_handles`）。`kernel32!OpenProcess` 全 w95 可（`tools/api_check`
     * 的 w95 段里有它）；9x 不认 `PROCESS_QUERY_INFORMATION` ⇒ 去掉它再试一次 —— 与
     * `util.cpp` 里 `mapped_name_from_psapi` 的降级同一口径。 */
    HANDLE h = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION,
                           FALSE, pid);
    if (h == NULL)
        h = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, pid);
    if (h == NULL)
    {
        E("OpenProcess(%lu)", (unsigned long)pid);
        return NULL;
    }
    for (int i = 0; i < kMaxPids; i++)
    {
        if (m_pid_handles[i].pid == 0)
        {
            m_pid_handles[i].pid    = pid;
            m_pid_handles[i].handle = h;
            return h;
        }
    }
    /* 缓存表满（与 pid 集合同一上限）：不缓存就该关掉 —— 漏一个句柄比“读不到”更糟。 */
    CloseHandle(h);
    E("OpenProcess(%lu): handle table full", (unsigned long)pid);
    return NULL;
}

void DebuggerSession::close_pid_handle(DWORD pid) noexcept
{
    for (int i = 0; i < kMaxPids; i++)
    {
        if (m_pid_handles[i].pid != pid) continue;
        if (m_pid_handles[i].handle) CloseHandle(m_pid_handles[i].handle);
        m_pid_handles[i].pid    = 0;
        m_pid_handles[i].handle = NULL;
        return;
    }
}

/* 全关 + 把树也作废：调用点只有 `begin`（同一会话重用时的第二轮）与 `run_shutdown`/析构。 */
void DebuggerSession::close_pid_handles() noexcept
{
    for (int i = 0; i < kMaxPids; i++)
    {
        if (m_pid_handles[i].handle) CloseHandle(m_pid_handles[i].handle);
        m_pid_handles[i].pid    = 0;
        m_pid_handles[i].handle = NULL;
    }
    m_pid_count = 0;
}

DWORD find_pid_by_mouse(const char **err)
{
    PrintS(TEXT("鼠标移至目标窗口，按 Ctrl 锁定...\n"));
    HANDLE                     hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(hStdOut, &csbi))
    {
        E("GetConsoleScreenBufferInfo");
        return 0;
    }
    csbi.dwCursorPosition.X = 0;
    size_t strn             = (csbi.srWindow.Bottom + 1 - csbi.dwCursorPosition.Y) * csbi.dwSize.X;
    LPTSTR str = AllocX(TCHAR, strn);
    if (!str)
        return 0;
    for (size_t i = 0; i < strn; i++) str[i] = TEXT(' ');
    HWND last = NULL;
    while (!(GetAsyncKeyState(VK_CONTROL) & 0x8000))
    {
        POINT pt;
        GetCursorPos(&pt);
        HWND h = LjGetRootWindow(WindowFromPoint(pt));
        if (h && h != last && h != LjGetConsoleWindow())
        {
            last = h;
            FlashWin(h);
            RECT r;
            GetWindowRect(h, &r);
            DWORD d;
            if (!WriteConsoleOutputCharacter(hStdOut, str, (DWORD)strn, csbi.dwCursorPosition, &d))
            {
                E("WriteConsoleOutputCharacter(' ' * %lu)", (DWORD)strn);
                return 0;
            }
            if (!SetConsoleCursorPosition(hStdOut, csbi.dwCursorPosition))
            {
                E("SetConsoleCursorPosition");
                return 0;
            }
            PrintF("> HWND:%p", h);
            int n = GetWindowTextLength(h);
            if (n > 0)
            {
                LPTSTR s = AllocX(TCHAR, n + 1);
                if (s)
                {
                    if (GetWindowText(h, s, n + 1) == n)
                    {
                        PrintS(" | Title:");
                        PrintN(s, n);
                    }
                    free_(s);
                }
            }
            PrintF(" | Pos:(%ld,%ld) Size:(%ldx%ld)", r.left, r.top, r.right - r.left, r.bottom - r.top);
        }
        Sleep(50);
    }
    free_(str);

    if (!last)
    {
        /* 旧实现：lua_pushliteral + lua_error（长跳）。现改由绑定层在同一位置 lua_error */
        if (err)
            *err = "未选中任何窗口";
        return 0;
    }

    DWORD pid;
    GetWindowThreadProcessId(last, &pid);
    PrintF(TEXT("\n已锁定 PID: %lu\n"), pid);
    return pid;
}
