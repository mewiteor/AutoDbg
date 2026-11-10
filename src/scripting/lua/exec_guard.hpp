#ifndef AUTODBG_SCRIPTING_LUA_EXEC_GUARD_HPP
#define AUTODBG_SCRIPTING_LUA_EXEC_GUARD_HPP
/* =============================================================================
 * src/scripting/lua/exec_guard.hpp —— 一次远程执行的取消 / 超时守卫（S2）
 *
 * 来源：新写（plan_server.md S2 / §5.4）。
 * 为什么需要单独一层：取消有**两条**完全不同的注入路径，必须共用同一份状态：
 *   ① 引擎阻塞在 `WaitForDebugEvent` 里时 → 只能靠 `DebugCallbacks::on_wait_timeout`
 *      （100 ms 粒度）问宿主「怎么办」。原实现是读控制台（`CheckConsoleInput`），
 *      而**无控制台时它会返回 1（分离并终止）**，在 server 里等于把被调试进程杀掉。
 *      → 所以 server 模式下必须先问本守卫（见 bindings.cpp 的 `lua_on_wait_timeout`）。
 *   ② 纯 Lua 死循环（不碰引擎）→ 只有 `lua_sethook` 的计数钩子能插话。
 * 状态机（全部 volatile/Interlocked，不依赖锁）：
 *   `guard_exec_begin(timeout)` 记录「本次代次」与截止时刻 → `guard_should_stop()`
 *   被两条路径轮询 → 首次命中就定下原因（cancel / timeout）→ 结果里报告出来。
 * ❗不引入静态 TLS（手工映射模块里没有 TLS 目录）：这些状态都是**模块级**的，
 *   而 server 的执行是严格串行的（并发 1），所以不需要 per-thread 副本。
 * ========================================================================== */
#include <windows.h>

#include <lua.hpp> /* 本文件属 Lua 层（core/common 不得 include）；用真声明而**不是**
                    * 自己 forward-declare `lua_State` —— 那会在 namespace 里造出一个
                    * 另一个类型（S2 实测：`autodbg::host::lua_State` 与全局的不同）。 */

namespace autodbg::host
{
enum guard_reason
{
    guard_none    = 0,
    guard_cancel  = 1,
    guard_timeout = 2,
};

/* 由 worker 在一次执行开始/结束时调用（串行，不可重入） */
void guard_exec_begin(unsigned timeout_sec);
void guard_exec_end();

/* 是否正处于一次远程执行中（绑定层据此决定「问谁」） */
bool guard_exec_active();

/* `/cancel`：只对**当前正在执行**的这次生效（没有执行时什么也不做） */
void guard_request_cancel();

/* 两条注入路径都轮询这个：命中即返回 true（首次命中时定下原因） */
bool guard_should_stop();

/* guard_none / guard_cancel / guard_timeout（给响应里的 canceled / timed_out 用） */
int guard_stop_reason();

/* 停止原因的可读文本（给 Lua 错误信息用，ASCII） */
const char *guard_stop_message();

/* Lua 计数钩子：命中就 `lua_error`（长跳转，不返回） */
void guard_lua_hook(lua_State *L, lua_Debug *ar);

/* 安装计数钩子（每 100 万条指令查一次；两个 Lua 版本都支持 LUA_MASKCOUNT） */
void guard_install_lua_hook(lua_State *L);
} /* namespace autodbg::host */

#endif /* AUTODBG_SCRIPTING_LUA_EXEC_GUARD_HPP */
