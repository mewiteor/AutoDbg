#ifndef AUTODBG_SCRIPTING_LUA_CONSOLE_INPUT_HPP
#define AUTODBG_SCRIPTING_LUA_CONSOLE_INPUT_HPP
/* =============================================================================
 * src/scripting/lua/console_input.hpp —— 宿主层的控制台输入策略（Q27 / P3.B-7）
 *
 * 归属：**宿主层**（不是引擎）。`run` 在 `WaitForDebugEvent` 超时时通过
 *   `DebugCallbacks::on_wait_timeout` 问宿主「继续等 / 分离并终止 / 分离但保留目标」，
 *   而「按 'Q' 退出、按住 Shift 保留目标」这条策略就实现在这里。
 *   （原 `cdebugger.c` 的 `CheckConsoleInput`，Q27 前计划搬进 `core/debug_loop.cpp`。）
 *
 * 为什么要单独一个文件：把「读控制台」与「判定」分开后，**判定是纯函数**，
 *   于是测试可以按「引擎 / 宿主」两条线各自独立地做（见 TODO 7.x）：
 *   · 宿主侧判定 → tests/unit/test_console_input.cpp（**不需要控制台、不需要注入按键**）；
 *   · 引擎侧「拿到结论后怎么做」→ 归模块 9（`run` 进 core 之后用假回调覆盖）。
 * 因此本文件**不依赖 Lua**、也不依赖 core 的其他模块（只用 `<windows.h>`），
 *   这样才能被 `tests_unit` 单独链接。
 * ========================================================================== */
#include <windows.h>

/* 扫描控制台输入记录，得出宿主结论（旧 `CheckConsoleInput` 的「扫描」段，逐字搬出）：
 *   返回值
 *     0: continue               没按 'Q'（或记录为空 / 非按键事件）
 *     1: detach and term        按了 'Q'（抬起）
 *     2: detach and not term    按了 'Q' 且按住 Shift，且 can_detach_without_term
 * 注：判定用的是 `wVirtualKeyCode == 'Q'` —— **小写 q 与大写 Q 是同一个虚拟键码**
 *   （Shift/CapsLock 只影响 `uChar.UnicodeChar`），所以 q / Q 都能退出。
 *   `can_detach_without_term` 为假时（引擎拿不到 `DebugActiveProcessStop`）恒返回 1。 */
int decide_console_action(const INPUT_RECORD *records, DWORD count, bool can_detach_without_term);

#endif /* AUTODBG_SCRIPTING_LUA_CONSOLE_INPUT_HPP */
