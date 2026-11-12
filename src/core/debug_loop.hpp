#ifndef AUTODBG_CORE_DEBUG_LOOP_HPP
#define AUTODBG_CORE_DEBUG_LOOP_HPP
/* =============================================================================
 * src/core/debug_loop.hpp —— 调试事件主循环（P3.B 模块 9）
 *
 * `run` 是引擎的最外层：`WaitForDebugEvent` 主循环 + 事件分发 + 调用宿主回调。
 * 它**不认识 Lua**：所有对外通知都走 `DebugCallbacks`（函数指针 + `void*` 上下文）。
 * `lua_on_*` 适配器、`LuaDebugAdapter`、`pcall` 仍留在 `scripting/lua/bindings.cpp`。
 *
 * ❗主循环仍是阻塞的、回调 1:1（Q26：当时不做 pump/协程/预留）；T7.1（plan_t7.md §2.1）把同一次
 *   派发拆成可调用的状态机（`begin` / `pump` / `cont` / `end`），`run` 是它们的封装。
 * ❗错误**不用长跳**表达（D18）：内部错误返回 `-1` 并置 `*err`，由绑定层在返回后
 *   `lua_error`；这样走完清理尾部，不跨 C++ 帧 longjmp。
 * ❗空闲轮询（Q27）：`WaitForDebugEvent` 超时（100ms 没等到事件）时调
 *   `DebugCallbacks::on_wait_timeout(ctx, can_detach_without_term)`，引擎只执行结论
 *   （继续等 / 分离并终止 / 分离但保留目标）——控制台策略在宿主层。
 * ========================================================================== */
#include "core.hpp"   /* P6.4：引擎函数已是 DebuggerSession 成员（下面的过渡转发也要它） */
#include "types.hpp"
#include "core.hpp"   /* P6.4：引擎函数已是 DebuggerSession 成员（下面的过渡转发也要它） */
#include "types.hpp" /* autodbg::core::{RunParams, DebugCallbacks} */

/* 调试会话主循环（阻塞）。返回 0 = 正常结束；-1 = 引擎内部错误（`*err` 指向静态字符串）。
 * 注意：返回 0 也可能是「附加失败就提前返回」（1:1 保留旧行为，不报 Lua 错误）。*/

/* 声明都在 `core.hpp`（P6.4：引擎函数是 `DebuggerSession` 成员）；本头只留约定。
 * ❗回调里不能重入 `pump`（plan_t7.md §2.3）——回调正在被 `pump` 派发。 */



#endif /* AUTODBG_CORE_DEBUG_LOOP_HPP */
