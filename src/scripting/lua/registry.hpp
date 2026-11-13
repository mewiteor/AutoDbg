#ifndef AUTODBG_SCRIPTING_LUA_REGISTRY_HPP
#define AUTODBG_SCRIPTING_LUA_REGISTRY_HPP
/* =============================================================================
 * src/scripting/lua/registry.hpp —— Lua 注册表（P4.2）
 *
 * 注册「脚本能看到的东西」：全局函数、`Var` / `FindMemoryContext` 元表、LuaVariant 元方法。
 * 实现见 registry.cpp（从 bindings.cpp 原样搬出）。
 * ========================================================================== */
#include <lua.hpp>

/* 注册「脚本能看到的东西」。
 * `repl = true`：REPL 口径 —— **不注册全局 `V`**（T7 切片 C1d：`V()` 只在调试回调里可用，
 *   顶层/REPL 里它只会报固定错误；`Var` 元表与 `d:v(addr)` 照旧在）。其余 28 个契约名、
 *   `gui` / `dbg` / `help` / `open_file` / `_autodbg_info` 一模一样。 */
void register_lua_api(lua_State *L, bool repl = false);

#endif /* AUTODBG_SCRIPTING_LUA_REGISTRY_HPP */