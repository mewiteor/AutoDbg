#ifndef AUTODBG_SCRIPTING_LUA_DBG_BINDINGS_HPP
#define AUTODBG_SCRIPTING_LUA_DBG_BINDINGS_HPP
/* =============================================================================
 * src/scripting/lua/dbg_bindings.hpp —— `dbg` 表（T7.3 / plan_t7.md §2.1）
 *
 * 来源：新写。形态照 `gui`：注册**一个全局表**（AGENTS.md §5 第 6 条），29 个契约名一个都不动。
 *   表内容：begin / pump / cont / close / step / eip / arm / hits / dropped / setcap / code / stopped / paused。`end` 用 `close` 的名字 —— `end` 是 Lua 关键字，`dbg.end` 写不出来（只有 `dbg["end"]()` 能调，所以不用）。
 *   便捷层（until / on_hit / 条件断点）是 Lua sugar，归 T7.6（新文件 + 新全局名），不在这里。
 *
 * 回调参数形状（新接口一律普通数字/字符串，不再用 LuaVariant）：`{ pid, tid, ea, code, modname, rva }`，
 *   与 `dbg.hits()` 的记录同名同义。`begin` 接受的选项键与 `Run` 的表格同名：
 *   `imagepath` / `cmdline` / `cwd` / `pid` / `is_new_console` / `use_scylla_hide`，加六个回调
 *   （`callback` / `exception_callback` / `on_exception` / `on_breakpoint` / `on_single_step` / `on_exit_process`）。
 *
 * 两条固定文案（门禁逐字断）定在本层：
 *   "pump cannot be called from a callback"      —— 回调里调状态机
 *   "cannot run while an event is unhandled"     —— `dbg` 停着一个未应答事件时调 `Run`/`Attach`
 * （后者被 bindings.cpp 的 `lua_run` / `lua_attach` 用：两条路径在同一个会话上不可混用。）
 * ========================================================================== */
#include <lua.hpp>

namespace autodbg::host
{
void register_dbg_api(lua_State *L);

bool        dbg_paused() noexcept;
const char *dbg_error_reentry() noexcept;
const char *dbg_error_paused()  noexcept;

} /* namespace autodbg::host */

#endif /* AUTODBG_SCRIPTING_LUA_DBG_BINDINGS_HPP */