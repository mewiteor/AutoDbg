/* =============================================================================
 * src/scripting/lua/namespace.cpp —— 目录骨架锚点（P1）
 *
 * 本目录 = `autodbg::scripting::lua`：**Lua 绑定层**（唯一允许感知 lua_State 的地方）。
 *   bindings（29 个 lua_* 注册函数）、registry（注册表与元表）、state（脚本加载与执行）、
 *   helpers（参数编组/错误文案）、variant（LuaVariant）、bson、main（入口 + CRT stub）。
 *
 * 约定：
 *   * Lua 头统一用 `<lua.hpp>`（`extern "C"` 包裹；`xmake.lua` 的 onload_lua_src 已生成）；
 *   * 本层只做「参数编组 → 调用 core → 结果编组」，不写业务逻辑；
 *   * 与 core 之间只经 `DebugCallbacks`（见 Plan.md 4.4/5.3）。
 *
 * 本文件只是骨架（验证构建系统），P4 起会被真实实现替换。
 * ========================================================================== */

namespace autodbg::scripting::lua
{
static_assert(__cplusplus >= 202002L, "需要 C++20（xmake.lua 按扩展名选择 /std:）");

const char *module_name()
{
    return "scripting::lua";
}
}
