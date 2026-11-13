/* =============================================================================
 * src/scripting/lua/help_bindings.hpp —— 全局 `help([topic])`（T7.3 切片 A / D53）
 *
 * 归属：宿主层（要 `lua_State` 才能做参数检查与返回值）。正文在 `help_bindings.cpp` 的一张
 *   静态表里 —— **一处文本、多处入口**：`help()`（索引）、`help('dbg')`（分组）、
 *   `help('dbg.pump')`（单条）、`dbg.help()`（等价于 `help('dbg')`）。
 *
 * ❗与 D53 的分工：`help` 只回答「现在该敲什么」（人类散文）。机器的「这台机器有什么」归
 *   `dbg.caps()`；API 语义 / 前置条件 / 坑归 skill 包（`tools/agent/autodbg-skill/`）。
 *   三者互相不复制 —— 否则必然漂移。
 * ========================================================================== */
#ifndef AUTODBG_SCRIPTING_LUA_HELP_BINDINGS_HPP
#define AUTODBG_SCRIPTING_LUA_HELP_BINDINGS_HPP

#include <lua.hpp>

namespace autodbg::host
{
/* 注册全局 `help`（新名字，不在 29 个契约名里）。 */
void register_help_api(lua_State *L);

/* 打印某个主题，返回 false = 没有这个主题（`dbg.help()` 用它，找不到时报固定文案）。
 * 输出走 core 的 `PrintS`（⇒ server 模式下进 `out` 帧，本地进控制台/管道）。 */
bool help_print_topic(const char *topic);

/* 打印索引（`help()` 无参时的那一屏）。 */
void help_print_index();
} /* namespace autodbg::host */

#endif /* AUTODBG_SCRIPTING_LUA_HELP_BINDINGS_HPP */
