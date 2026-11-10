#ifndef AUTODBG_SCRIPTING_LUA_HELPERS_HPP
#define AUTODBG_SCRIPTING_LUA_HELPERS_HPP
/* =============================================================================
 * src/scripting/lua/helpers.hpp —— Lua 绑定层的参数编组工具（Plan 5.7 / P3.A.3）
 *
 * 来源：`src/debugger/lib.c` 的 **Lua 相关部分**。与 `core/util.hpp` 的拆分依据：
 *   只要函数签名/实现依赖 `lua_State`（或只服务于参数编组），就属于本层。
 *   `str2integer` 本身不依赖 Lua，但只被 LuaVariant 的字符串构造使用，按 5.7 归此层。
 *
 * Lua 头统一用 `<lua.hpp>`（`extern "C"` 包裹；由 xmake 的 onload_lua_src / after_build_luajit
 * 生成到 `$(builddir)/{lua,luajit}/include/`），以同时兼容 Lua 5.5 与 LuaJIT。
 * ========================================================================== */
#include <windows.h>
#include <stdbool.h>
#include <lua.hpp>

namespace autodbg::scripting::lua
{
/* Lua 字符串 → 新分配的 TCHAR 串（调用方负责 autodbg::common::release）
 *   Unicode 构建：UTF-8 → UTF-16；ANSI 构建：_strdup（原样） */
LPTSTR get_lua_str(lua_State *L, int idx);

/* 生成 "期望 <tname>，实际 <实际类型>" 的参数类型错误（抛 Lua error） */
int lua_type_error(lua_State *L, int arg, const char *tname);

/* 字符串 → 整数：bs ∈ {1,2,4,8} 字节宽度，sign 决定是否有符号；
 *   支持前导空白、+/- 号、0x/0 前缀（16/8 进制）。结果写入 v（bs 字节）。 */
void str2integer(const char *s, int bs, bool sign, void *v);

/* ---------------------------------------------------------------- 回调派发辅助（P4.3）
 * 来源：`bindings.cpp`（原样搬入）。宿主适配器 `lua_on_*` 用它把用户回调包在
 * `debug.traceback` + `lua_pcall` 里，并把 Lua 错误收敛成日志（**不重抛** → 回调不会
 * 长跳穿引擎，见 Plan C.10 / D18）。
 * `pcall` 的 `file` 参数就是调用点的 `__FILE__`（宏传入），保证日志里的文件名与旧实现一致。 */

/* 压入 `debug.traceback` 并返回当前栈顶（作为 `lua_pcall` 的 msg handler 位置） */
int pcall_pre(lua_State *L);

/* 包装 `lua_pcall`：成功返回 true；失败记日志、恢复栈顶、返回 false（不抛） */
bool pcall(lua_State *L, int narg, int nresult, int oldtop, LPCTSTR file, int line);

#define PCALL(L, narg, nresult, oldtop) pcall(L, narg, nresult, oldtop, TEXT(__FILE__), __LINE__)

/* 引擎→Lua 回调的嵌套深度（`pcall()` 内部维护）。
 * >0 表示当前在 **C 调用边界内**：此时 `lua_yield` 非法（LuaJIT 根本不支持；
 * Lua 5.5 也不允许跨普通 `lua_pcall`）——`read_file`/`write_file` 靠它提前给出
 * 可操作的错误，而不是让 Lua 抛 "attempt to yield across a C-call boundary"
 * （那会直接打断整个回调/调试会话，用户只能看到 traceback）。 */
int callback_depth();
}

#endif /* AUTODBG_SCRIPTING_LUA_HELPERS_HPP */
