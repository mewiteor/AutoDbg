/* =============================================================================
 * src/common/console_action.hpp —— 宿主层控制台输入的「判定」（纯函数，Q27 / T7.3 搬家）
 *
 * 来源：`src/scripting/lua/console_input.hpp`（T7.1 之前就在那儿）。T7.3 搬进 `src/common/`：
 *   `run` 在 `WaitForDebugEvent` 超时时通过 `DebugCallbacks::on_wait_timeout` 问宿主
 *   「继续等 / 分离并终止 / 分离但保留目标」，而「按 Q 退出、按住 Shift 保留目标」这条策略
 *   原本只被判定的那半个函数。
 *
 * 为什么在 `common/`：这半截是**纯判定**（不做 I/O、不碰 Lua、只用 `<windows.h>`），
 *   而消费它的是两处：绑定层 `bindings.cpp`（真的去读控制台记录）与 `common/selftest.cpp`
 *   （按记录列表直接断言）。放在 `scripting/lua/` 会让 `common → scripting` 的反向依赖成立
 *   （plan_t7.md §2.1 记录 ①），所以 T7.3 一并理顺：
 *     · 读控制台（`ReadConsoleInput`）留在绑定层（`bindings.cpp`）；
 *     · 判定（本文件）归 `common/`，可单测。
 *
 * 口径（返回值，与旧 `CheckConsoleInput` 逐字一致）：
 *   0 = 继续等；1 = 分离并终止；2 = 分离但保留目标（仅当 can_detach_without_term 且按住 Shift）。
 * 注：判定用 `wVirtualKeyCode == 'Q'` —— 小写 q 与大写 Q 是同一个虚拟键码，
 *   所以 q / Q 都能退出；`can_detach_without_term` 为假时恒返回 1。
 * ========================================================================== */
#ifndef AUTODBG_COMMON_CONSOLE_ACTION_HPP
#define AUTODBG_COMMON_CONSOLE_ACTION_HPP

#include <windows.h>

namespace autodbg::common
{
/* 扫描控制台输入记录，得出宿主结论（旧 `CheckConsoleInput` 的「扫描」段，逐字搬出）。 */
int decide_console_action(const INPUT_RECORD *records, DWORD count, bool can_detach_without_term);
} /* namespace autodbg::common */

#endif /* AUTODBG_COMMON_CONSOLE_ACTION_HPP */
