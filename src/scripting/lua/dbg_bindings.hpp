#ifndef AUTODBG_SCRIPTING_LUA_DBG_BINDINGS_HPP
#define AUTODBG_SCRIPTING_LUA_DBG_BINDINGS_HPP
/* =============================================================================
 * src/scripting/lua/dbg_bindings.hpp —— `dbg` 表 + 会话对象（T7.3 / 切片 C1a）
 *
 * 来源：新写。形态照 `gui` / `open_file`：注册**一个全局表** `dbg`（AGENTS.md §5 第 6 条），
 *   29 个契约名一个都不动。
 *
 * 形状（切片 C1a：D55 / D57，仍限**单会话**）：
 *   · `dbg.create{...}` / `dbg.attach(pid)` → **会话对象**（full userdata + metatable `AutoDbgSession`）；
 *   · 对象方法：pump / cont / step / eip / arm / hits / dropped / setcap / stopped / paused / caps /
 *     help / detach / kill（`detach` 只在平台有 `DebugActiveProcessStop` 时装，D51）；
 *   · 对象字段：`stop`（当前未应答的事件表，与 `pump` 返回的是**同一张表**）/ `root_pid` / `kind` /
 *     `state`（`running` / `stopped` / `exited`）；
 *   · 模块级保留 create / attach / pump / cont / step / eip / arm / hits / dropped / setcap / stopped /
 *     paused / detach / kill / caps / help —— 作用对象是**默认会话**（最近一次 create/attach 的那个）；
 *   · registry 查询：`dbg.sessions()` / `dbg.session(pid)` / `dbg.last()`（D54）；
 *   · `dbg.code()` 已删：事件表带 `code`（异常码）与 `cont_code`（core 算好的默认应答码）。
 *
 * 回调参数形状（新接口一律普通数字/字符串，不再用 LuaVariant）：`{ pid, tid, ea, code, modname, rva }`，
 *   与 `dbg.hits()` 的记录同名同义。回调键在**两个入口共用**：
 *   `callback` / `exception_callback` / `on_exception` / `on_breakpoint` / `on_single_step` / `on_exit_process`
 *   （`attach` 不给选项表 ⇒ 不要回调，只记 `hits()`）。
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

/* `dbg_paused()` 查**默认会话**的 `d.stop`（`Run`/`Attach` 的门禁；没有会话 = 没停着事件）。 */
bool        dbg_paused() noexcept;
const char *dbg_error_reentry() noexcept;
const char *dbg_error_paused()  noexcept;

} /* namespace autodbg::host */

#endif /* AUTODBG_SCRIPTING_LUA_DBG_BINDINGS_HPP */
