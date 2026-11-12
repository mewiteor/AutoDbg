#ifndef AUTODBG_SCRIPTING_LUA_STATE_HPP
#define AUTODBG_SCRIPTING_LUA_STATE_HPP
/* =============================================================================
 * src/scripting/lua/state.hpp —— Lua 状态与脚本加载（P4.1，从 bindings.cpp 搬出）
 *
 * 职责（Plan 5.7 / Q7：「脚本 / 工作目录相关，与调试无关」）：
 *   · `run_script_file()`：建 `lua_State` → `register_lua_api()` → 加载烘焙库
 *     （`all_bytecodes`）→ `my_loadfile()` 加载脚本 → 传脚本参数 → `lua_pcall`
 *     → 出错时按旧格式打印 → `lua_close`；返回旧 `_tmain` 的退出码。
 *   · `my_loadfile()`：脚本文件（源码或字节码）→ Lua 函数（原样搬入）。
 *   · `g_wfilepath`：脚本路径（**Q7 归 Lua 层**，不进 `DebuggerSession`）。
 *
 * 注：原先的 `generate_templete()`（往脚本目录旁写 `cdebugger.d.lua` + `.luarc.json`）
 *   已移除 —— 编辑器提示改由 neovim 插件的 LuaLS 注解库提供（插件仓库
 *   `addons/autodbg/` + `lua/autodbg/lsp.lua`），模块不再往脚本目录写文件。
 * ========================================================================== */
#include <windows.h>
#include <lua.hpp>

/* 脚本路径（原 bindings.cpp 的定义 → 这里；保持 C 链接，与旧引用方式一致） */
extern "C"
{
extern LPCTSTR g_wfilepath;
}

/* 加载脚本（源码或字节码）为栈顶的 Lua 函数；返回 Lua 状态码（0 = 成功） */
int my_loadfile(lua_State *L, LPCTSTR path);

/* 脚本目录工具（Q7）：清空目录内容后重建；Lua 侧 `ResetDir` 的实现
 * （P4.6 从 bindings.cpp 搬来：它只操作脚本目录，与调试无关） */
int lua_reset_dir(lua_State *L);

/* 建状态 → 注册 → 烘焙库（与 run_script_file 前半段逐字一致；S0 的 server/exec 用）
 * 返回 nullptr = `luaL_newstate()` 失败。 */
lua_State *create_state_with_libs(void);

/* 建状态 → 注册 → 烘焙库 → 加载并运行 `g_wfilepath`（argv[2..] 作为脚本参数）。
 * 返回退出码（旧 `_tmain` 语义：0 = 成功，-1 = 加载/运行失败）。
 * T1（D50）的可选出参 `result_json`：只在 `--json` 模式用 —— 在 `lua_close` **之前**
 *   把脚本的 `return` 值交给烘焙的 `jsonx.encode`（C 侧不写 parser，D28），把结果
 *   拷进 `common::alloc` 的缓冲区交回调用方（调用方负责 release）。传 NULL → 行为与
 *   旧实现逐字节一致。口径：1 个返回值 → 那个值；0 个 → `null`；多个 → 数组。
 *   编不了（function / 无 `val` 的 userdata / 稀疏表 / NaN / 循环引用）→ `null`（pcall
 *   包住，不把 Lua 错误抛出去）—— 不改 `jsonx` 本身。 */
int run_script_file(int argc, LPTSTR argv[], const char **result_json = nullptr);
#endif /* AUTODBG_SCRIPTING_LUA_STATE_HPP */
