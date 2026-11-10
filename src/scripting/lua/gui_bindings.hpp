#ifndef AUTODBG_SCRIPTING_LUA_GUI_BINDINGS_HPP
#define AUTODBG_SCRIPTING_LUA_GUI_BINDINGS_HPP
/* =============================================================================
 * src/scripting/lua/gui_bindings.hpp —— `gui` 表的 Lua 绑定（S4）
 *
 * 来源：新写（plan_server.md S4 / §7.1，决策 D23）。
 * 形态：注册**一个全局表** `gui`（不做 25+ 个平铺全局），方法见 gui_bindings.cpp。
 *   既有的 29 个契约全局名（Run/ReadMemory/…）一个字不改。
 * 分工：本文件只做参数编组 + UTF-8 ↔ TCHAR 转换；实际动作在 `src/core/gui.cpp`。
 * ========================================================================== */
#include <lua.hpp>

/* 在 `register_lua_api()` 里调用一次 */
void register_gui_api(lua_State *L);

#endif /* AUTODBG_SCRIPTING_LUA_GUI_BINDINGS_HPP */
