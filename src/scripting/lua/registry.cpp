/* =============================================================================
 * src/scripting/lua/registry.cpp —— Lua 注册表（P4.2，从 bindings.cpp 原样搬出）
 *
 * `register_lua_api()` = 29 个全局函数（`lua_register`）+ `Var` 元表方法 +
 *   `FindMemoryContext` 的 `__gc` + `lv_init(L)`（LuaVariant 元方法在 variant.cpp 里注册）。
 * 函数体**逐字**来自 bindings.cpp 的 `_tmain`，只是收进一个函数、加了缩进。
 * 依赖：bindings.hpp（那些 `lua_*` 入口）、variant.hpp（`lv_init`）。
 * ========================================================================== */
#include "registry.hpp"

#include "bindings.hpp"
#include "bson.hpp"  /* lua_bson_encode / lua_bson_decode */
#include "file_io.hpp"     /* S7：read_file / write_file（client 文件 I/O） */
#include "gui_bindings.hpp" /* S4：`gui` 表（plan_server.md §7.1） */
#include "state.hpp" /* lua_reset_dir（Q7：脚本目录工具，声明在 state.hpp） */
#include "variant.hpp"

#include "../../sys/pack/boot.h" /* AUTODBG_ABI_VERSION */
#include "../../sys/version.h"   /* AUTODBG_VERSION（构建期生成，见 make.bat :genversion） */

namespace
{
/* `_autodbg_info()` —— 宿主自己的身份（Lua 层拿不到编译期宏；MCP 的 `serverInfo.version`
 * 与 `dbg_status` 工具靠它，否则只能写死 "0.1.0"）。
 * 返回：{ version, version_source, abi, flavor, variant, pid, client_io } */
int lua_autodbg_info(lua_State *L)
{
    lua_createtable(L, 0, 7);

    lua_pushstring(L, AUTODBG_VERSION);
    lua_setfield(L, -2, "version");
    lua_pushstring(L, AUTODBG_VERSION_SOURCE);
    lua_setfield(L, -2, "version_source");
    lua_pushinteger(L, (lua_Integer)AUTODBG_ABI_VERSION);
    lua_setfield(L, -2, "abi");

#ifdef JIT
    lua_pushliteral(L, "luajit");
#else
    lua_pushliteral(L, "lua");
#endif
    lua_setfield(L, -2, "flavor");

#ifdef NO_SCYLLA_HIDE
    lua_pushliteral(L, "legacy");
#else
    lua_pushliteral(L, "modern");
#endif
    lua_setfield(L, -2, "variant");

    lua_pushinteger(L, (lua_Integer)GetCurrentProcessId());
    lua_setfield(L, -2, "pid");

    lua_pushboolean(L, autodbg::host::client_io_enabled() ? 1 : 0);
    lua_setfield(L, -2, "client_io");
    return 1;
}
} /* namespace */

void register_lua_api(lua_State *L)
{
    lua_register(L, "Run", lua_run);
    lua_register(L, "Attach", lua_attach);
    lua_register(L, "Detach", lua_detach);
    lua_register(L, "Kill", lua_kill);
    lua_register(L, "ForceKill", lua_force_kill);
    lua_register(L, "SetBreakpoint", lua_set_breakpoint);
    lua_register(L, "DelBreakpoint", lua_del_breakpoint);
    lua_register(L, "SetHardwareBreakpoint", lua_set_hardware_breakpoint);
    lua_register(L, "DelHardwareBreakpoint", lua_del_hardware_breakpoint);
    lua_register(L, "ReadMemory", lua_read_memory);
    lua_register(L, "WriteMemory", lua_write_memory);
    lua_register(L, "V", lua_v);
    lua_register(L, "GetContext", lua_get_context);
    lua_register(L, "SetContext", lua_set_context);
    lua_register(L, "Ea2ModnameRva", lua_ea2_modname_rva);
    lua_register(L, "GetModBase", lua_get_mod_base);
    lua_register(L, "StackTrace", lua_stack_trace);
    lua_register(L, "SetBreakpoints", lua_set_breakpoints);
    lua_register(L, "EnableBreakpoint", lua_enable_breakpoint);
    lua_register(L, "DisableBreakpoint", lua_disable_breakpoint);
    lua_register(L, "LoadStr", lua_load_str);
    lua_register(L, "Hide", lua_hide);
    lua_register(L, "FindFuncRva", lua_find_func_rva);
    lua_register(L, "GetCppClassName", lua_get_cpp_class_name);
    lua_register(L, "FindMemory", lua_find_memory);
    lua_register(L, "GetMemoryRegion", lua_get_memory_region);
    lua_register(L, "ResetDir", lua_reset_dir);
    lua_register(L, "BsonEncode", lua_bson_encode);
    lua_register(L, "BsonDecode", lua_bson_decode);

    /* S4（plan_server.md §7.1 / D23）：GUI 自动化走**单一全局表** `gui`，
     * 上面 29 个契约全局名一个字不改。 */
    register_gui_api(L);

    /* S7：client 文件 I/O（server 模式下走效果协议，本机模式直接读写磁盘） */
    autodbg::host::register_file_api(L);

    /* 宿主身份（MCP 的 serverInfo.version / dbg_status 用；不污染 29 个契约名） */
    lua_register(L, "_autodbg_info", lua_autodbg_info);

    /* 注册M.Var相关函数 */
    luaL_newmetatable(L, "Var");
    lua_pushcfunction(L, lua_var_index);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, lua_var_to);
    lua_setfield(L, -2, "to");
    lua_pushcfunction(L, lua_var_tos);
    lua_setfield(L, -2, "tos");
    lua_pushcfunction(L, lua_var_sto);
    lua_setfield(L, -2, "sto");
    lua_pushcfunction(L, lua_var_write);
    lua_setfield(L, -2, "write");
    lua_pushcfunction(L, lua_var_writes);
    lua_setfield(L, -2, "writes");
    lua_pushcfunction(L, lua_var_swrite);
    lua_setfield(L, -2, "swrite");
    lua_pushcfunction(L, lua_var_off);
    lua_setfield(L, -2, "off");
    lua_pop(L, 1);

    lv_init(L);

    luaL_newmetatable(L, "FindMemoryContext");
    lua_pushcfunction(L, lua_find_memory_context_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
}
