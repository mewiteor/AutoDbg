#ifndef AUTODBG_SCRIPTING_LUA_BINDINGS_HPP
#define AUTODBG_SCRIPTING_LUA_BINDINGS_HPP
/* =============================================================================
 * src/scripting/lua/bindings.hpp —— Lua 绑定的**对外符号表**（P4.2）
 *
 * 这些 `lua_*` 函数是注册表（`registry.cpp`）与脚本状态（`state.cpp`）需要引用的入口。
 * 在 bindings.cpp 内部它们仍是「参数编组 → 调 core」的薄封装（P4.6 继续收窄）。
 * 注：**没有** `extern "C"` —— 它们是 C++ 函数，只有 Lua 的头是 C 链接。
 * ========================================================================== */
#include <lua.hpp>

/* 硬件断点长度的说明文本（x64 支持 1/2/4/8 字节，x86 只到 4）——绑定层的函数帮助文本
 * 与 `state.cpp` 生成的 `cdebugger.d.lua` 模板都要用 → 提到这里（原先定义在函数体内）。 */
#ifdef _WIN64
#   define HWB_SIZE_STR "1、2、4、8"
#else
#   define HWB_SIZE_STR "1、2、4"
#endif

/* 29 个注册给脚本的全局函数 + `Var` 元表方法 + `FindMemoryContext` 的 __gc
 * 注：`lua_reset_dir` 属「脚本目录工具」（Q7）→ 声明在 `state.hpp`，不在这里。 */
int lua_attach(lua_State *L);
int lua_del_breakpoint(lua_State *L);
int lua_del_hardware_breakpoint(lua_State *L);
int lua_detach(lua_State *L);
int lua_disable_breakpoint(lua_State *L);
int lua_ea2_modname_rva(lua_State *L);
int lua_enable_breakpoint(lua_State *L);
int lua_find_func_rva(lua_State *L);
int lua_find_memory(lua_State *L);
int lua_find_memory_context_gc(lua_State *L);
int lua_find_memory_iter(lua_State *L);
int lua_force_kill(lua_State *L);
int lua_get_context(lua_State *L);
int lua_get_cpp_class_name(lua_State *L);
int lua_get_memory_region(lua_State *L);
int lua_get_mod_base(lua_State *L);
int lua_hide(lua_State *L);
int lua_kill(lua_State *L);
int lua_load_str(lua_State *L);
int lua_read_memory(lua_State *L);
int lua_reset_dir(lua_State *L);
int lua_run(lua_State *L);
int lua_set_breakpoint(lua_State *L);
int lua_set_breakpoints(lua_State *L);
int lua_set_context(lua_State *L);
int lua_set_hardware_breakpoint(lua_State *L);
int lua_stack_trace(lua_State *L);
int lua_stack_trace_iter(lua_State *L);
int lua_v(lua_State *L);
int lua_var_index(lua_State *L);
int lua_var_off(lua_State *L);
int lua_var_sto(lua_State *L);
int lua_var_swrite(lua_State *L);
int lua_var_to(lua_State *L);
int lua_var_tos(lua_State *L);
int lua_var_write(lua_State *L);
int lua_var_writes(lua_State *L);
int lua_write_memory(lua_State *L);

#endif /* AUTODBG_SCRIPTING_LUA_BINDINGS_HPP */
