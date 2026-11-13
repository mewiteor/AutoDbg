/* =============================================================================
 * src/common/console_style.hpp —— `help` 上色的「判定」（纯函数 + 环境探测，T7.3 切片 B / D53）
 *
 * 来源：新写（plan_t7.md §11.3 / D53）。T7 切片 A 的 `help` 是纯文本；切片 B 只在**真控制台**
 *   上带颜色，其余情况（管道 / 重定向 / server 的 `out` 帧 / `--json` 收进信封）一律纯文本、
 *   零属性字节。不用 ANSI 转义：Win9x 控制台没有 VT（D53）。
 *
 * 为什么判定落在 `common/`：上色与否是**纯判定**（不做 I/O、不碰 Lua、只查一次环境变量），
 *   消费它的两处是绑定层 `help_bindings.cpp` 与 `common/selftest.cpp`。放进 `scripting/lua/`
 *   会让 `common -> scripting` 的反向依赖成立（与 `console_action.hpp` 搬家的同一条理由）。
 *   真正的属性设置与写句柄在 core 的 `print_styled()`（core 不认 Lua）。
 *
 * 导入表：`GetEnvironmentVariableA` 在 Win95 的 kernel32 里就有
 *   （`tools/api_check/datas.bin` 的 w95 表已核实），所以这里不需要 dynapi 降级。
 * ========================================================================== */
#ifndef AUTODBG_COMMON_CONSOLE_STYLE_HPP
#define AUTODBG_COMMON_CONSOLE_STYLE_HPP

#include <windows.h>

namespace autodbg::common
{
/* 是否给输出上色：`is_console && !no_color_env`。
 * `is_console` 由调用方探测（`GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode)` 成功
 *   = 真控制台），本函数因此是**纯函数**、可直接单测。 */
bool console_style_enabled(bool is_console, bool no_color_env);

/* 用户是否用 `NO_COLOR` 关掉了颜色（no-color.org 的口径：**存在且非空**即为真）。 */
bool no_color_requested();
} /* namespace autodbg::common */

#endif /* AUTODBG_COMMON_CONSOLE_STYLE_HPP */
