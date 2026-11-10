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
 * ========================================================================== */
#include "exec_guard.hpp"

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
