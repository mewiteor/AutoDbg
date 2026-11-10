#ifndef AUTODBG_SCRIPTING_LUA_REGISTRY_HPP
#define AUTODBG_SCRIPTING_LUA_REGISTRY_HPP
/* =============================================================================
 * src/scripting/lua/registry.hpp —— Lua 注册表（P4.2）
 *
 * 注册「脚本能看到的东西」：全局函数、`Var` / `FindMemoryContext` 元表、LuaVariant 元方法。
 * 实现见 registry.cpp（从 bindings.cpp 原样搬出）。
 * ========================================================================== */
#include <lua.hpp>

void register_lua_api(lua_State *L);

#endif /* AUTODBG_SCRIPTING_LUA_REGISTRY_HPP */