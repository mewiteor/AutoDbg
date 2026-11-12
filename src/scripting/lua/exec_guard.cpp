/* =============================================================================
 * src/scripting/lua/exec_guard.cpp —— 取消 / 超时守卫实现（S2）
 *
 * 来源：新写（plan_server.md S2）。
 * 与旧实现的差异：无（新文件）。
 * 实现要点：
 *   ① 只用 `volatile LONG` + `Interlocked*`（kill/看门狗/执行者三方都不持锁，
 *      避免「取消请求要等执行结束」的死结）；
 *   ② `GetTickCount` 的 ~49.7 天回绕用**有符号差值**处理（`(LONG)(now - deadline) >= 0`）；
 *   ③ 计数钩子每 100 万条指令触发一次 → 对正常脚本的开销可忽略（`lua_sethook` 本身
 *      在 LuaJIT 下也支持 LUA_MASKCOUNT）。
 *   ④ T1（D50）：多一个「目标是否存活」的标志（`guard_mark_detached` / `guard_detached`），
 *      给 `--json` 的 `detached` 字段。本地 CLI 不走 `guard_exec_begin` → 这份状态是进程级的
 *      （一个进程只跑一个脚本），不需要重置。
 * ========================================================================== */
#include "exec_guard.hpp"

#include "stream.hpp" /* D32：执行期的 control poll（`guard_poll_control` 的落点） */

#include <lua.hpp>

namespace autodbg::host
{
namespace
{
constexpr int k_hook_instruction_count = 1000000;

volatile LONG g_active     = 0; /* 1 = 有执行在进行 */
volatile LONG g_cancel_seq = 0; /* `/cancel` 计数（每次 /cancel +1） */
volatile LONG g_seq        = 0; /* 本次执行的基线（begin 时快照） */
volatile LONG g_stop       = 0; /* 0 = 无；1 = cancel；2 = timeout */
volatile LONG g_detached   = 0; /* T1（D50）：1 = 目标存活（真 detach） */
volatile LONG g_soft       = 0; /* D46：杀/分离的软动作（0 = 无；见 guard_soft_action） */
volatile DWORD g_deadline  = 0; /* 截止 tick；0 = 不限 */

/* 有符号比较：正确处理 GetTickCount 回绕 */
inline bool tick_reached(DWORD now, DWORD deadline)
{
    return (LONG)(now - deadline) >= 0;
}
} /* namespace */

void guard_exec_begin(unsigned timeout_sec)
{
    InterlockedExchange(&g_seq, InterlockedCompareExchange(&g_cancel_seq, 0, 0));
    InterlockedExchange(&g_stop, 0);
    InterlockedExchange(&g_soft, 0);       /* D46：新一次执行从无软动作开始 */
    InterlockedExchange(&g_detached, 0);   /* T1（D50） */

    if (timeout_sec != 0)
    {
        unsigned long long ms = (unsigned long long)timeout_sec * 1000ull;
        if (ms > 0x7FFFFFF0ull)
            ms = 0x7FFFFFF0ull; /* 半个 tick 周期以内，避免与回绕判据打架 */
        InterlockedExchange(&g_deadline, (DWORD)(GetTickCount() + (DWORD)ms));
    }
    else
    {
        InterlockedExchange(&g_deadline, 0);
    }
    InterlockedExchange(&g_active, 1);
}

void guard_exec_end()
{
    InterlockedExchange(&g_deadline, 0);
    InterlockedExchange(&g_active, 0);
}

bool guard_exec_active()
{
    return InterlockedCompareExchange(&g_active, 0, 0) != 0;
}

void guard_request_cancel()
{
    if (guard_exec_active())
        InterlockedIncrement(&g_cancel_seq);
}

bool guard_should_stop()
{
    LONG stop = InterlockedCompareExchange(&g_stop, 0, 0);
    if (stop != 0)
        return true;

    if (InterlockedCompareExchange(&g_cancel_seq, 0, 0) != InterlockedCompareExchange(&g_seq, 0, 0))
    {
        InterlockedExchange(&g_stop, (LONG)guard_cancel);
        return true;
    }

    DWORD deadline = g_deadline;
    if (deadline != 0 && tick_reached(GetTickCount(), deadline))
    {
        InterlockedExchange(&g_stop, (LONG)guard_timeout);
        return true;
    }
    return false;
}

int guard_stop_reason()
{
    return (int)InterlockedCompareExchange(&g_stop, 0, 0);
}

/* T1（D50）：`--json` 的 `detached`。只有「目标真的存活」才传 true（D47：Win95/98 取不到
 * `DebugActiveProcessStop` → 降级成 kill → 不算 detached）。 */
void guard_mark_detached(bool target_alive)
{
    InterlockedExchange(&g_detached, target_alive ? 1 : 0);
}

int guard_detached()
{
    return (int)InterlockedCompareExchange(&g_detached, 0, 0);
}

void guard_request_soft(int action)
{
    InterlockedExchange(&g_soft, (LONG)action);
}

int guard_take_soft()
{
    return (int)InterlockedExchange(&g_soft, 0);   /* 原子「取并清」：只会作用一次 */
}

void guard_poll_control()
{
    /* 实现在 stream.cpp（那里才有帧协议）。这里只是一个便于调用点统一的薄封装，
     * 也让 `exec_guard.cpp` 不必知道 stream 的存在。 */
    autodbg::host::stream_poll_and_dispatch();
}

const char *guard_stop_message()
{
    switch (guard_stop_reason())
    {
    case guard_cancel:  return "canceled by server (/cancel)";
    case guard_timeout: return "canceled by server (--server-timeout)";
    default:            return "canceled by server";
    }
}

void guard_lua_hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    /* D32 的第②个 poll 点：纯 Lua 死循环里只有这里能插话（不经过引擎）。
     * 先收控制帧（可能置 cancel / 软动作），再看要不要长跳。 */
    guard_poll_control();
    if (!guard_should_stop())
        return;
    /* 用 pushstring + lua_error（不用 luaL_error：避免格式串语义差异） */
    lua_pushstring(L, guard_stop_message());
    lua_error(L); /* 长跳转，不返回 */
}

void guard_install_lua_hook(lua_State *L)
{
    lua_sethook(L, guard_lua_hook, LUA_MASKCOUNT, k_hook_instruction_count);
}
} /* namespace autodbg::host */
