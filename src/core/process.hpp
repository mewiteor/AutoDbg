#ifndef AUTODBG_CORE_PROCESS_HPP
#define AUTODBG_CORE_PROCESS_HPP
/* =============================================================================
 * src/core/process.hpp —— 进程 / 线程控制（P3.B 模块 7）
 *
 * 从 `scripting/lua/bindings.cpp` 迁入（P3.B 模块 7），与旧实现**逐字一致**，
 * 只有两处接口调整：
 *   · `get_thread_by_tid` / `detach_process` 去掉 `static`（绑定层要调）；
 *   · `find_pid_by_mouse(lua_State*)` → `find_pid_by_mouse(const char **err)`
 *     —— 引擎不再感知 Lua：错误文案交还调用方，由绑定层 `lua_error`。
 *
 * ❗ **本模块不含「控制台输入策略」**：`CheckConsoleInput` 已按 Q27 移到宿主层
 *    （`scripting/lua/bindings.cpp`），由 `DebugCallbacks::on_wait_timeout` 回调驱动
 *    —— 引擎只执行宿主的结论（继续等 / 分离并终止 / 分离但保留目标）。
 *    （`Plan.md` 附录 A 与 `docs/refactor_symbol_map.md` 原本把 `CheckConsoleInput`
 *      列进 `core/debug_loop.cpp`，Q27 改为「宿主层 + 新回调通道」。）
 *
 * 依赖：memory（write）、pe（get_base）、engine（g_breakpoint_list）、util（打印）。
 * ========================================================================== */
#include "core.hpp"   /* P6.4：引擎函数已是 DebuggerSession 成员（下面的过渡转发也要它） */
#include "types.hpp"
#include "core.hpp"   /* P6.4：引擎函数已是 DebuggerSession 成员（下面的过渡转发也要它） */
#include "types.hpp"   /* autodbg::core::DebugCallbacks */

/* 按线程 id 查已知线程表（g_thread_list）里的句柄；未找到返回 NULL */

/* 分离目标：把激活断点的原字节写回 + 逐个 `on_breakpoint_destroyed` 通知 + 释放断点表。
 * ❗ 不关句柄、不碰 g_pi（那是 `run_shutdown` 的职责，1:1 保留旧分工）；
 *    `cb` 可为 NULL（调用方自己负责释放令牌）。 */

/* 交互式选目标：把鼠标移到窗口上按 Ctrl 锁定。
 * 返回 0 = 未选中 / 失败；`*err` **只**在「未选中任何窗口」时被设为静态错误文案
 * （其余失败路径静默返回 0，与旧实现 1:1：旧代码只有这一条路会 lua_error）。
 * `err` 可为 NULL。 */
DWORD find_pid_by_mouse(const char **err);



#endif /* AUTODBG_CORE_PROCESS_HPP */
