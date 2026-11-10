/* =============================================================================
 * src/core/namespace.cpp —— 目录骨架锚点（P1）
 *
 * 本目录 = `autodbg::core`：**纯调试引擎**。
 *   断点（软/硬）、进程/线程控制、内存读写与搜索、寄存器上下文、PE 解析、
 *   栈回溯、调试事件循环、反反调试（hide）、通用工具（util）。
 *
 * 约束（关键，见 Plan.md 验收项 #5 与 TODO 4.6）：
 *   * **禁止** `#include <lua.h>/<lua.hpp>/<lauxlib.h>/<lualib.h>`；
 *   * 与绑定层交互一律经**回调结构体**（`DebugCallbacks` + 回调句柄，见 4.4/5.5）；
 *   * 本目录**不得**感知 Lua：Lua 交互只发生在 `src/scripting/lua`。
 *
 * 本文件只是骨架（验证构建系统），P3 起会被真实模块（breakpoints/memory/pe/…）替换。
 * ========================================================================== */

namespace autodbg::core
{
static_assert(__cplusplus >= 202002L, "需要 C++20（xmake.lua 按扩展名选择 /std:）");

const char *module_name()
{
    return "core";
}
}
