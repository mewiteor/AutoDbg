#ifndef AUTODBG_SCRIPTING_LUA_REPL_HPP
#define AUTODBG_SCRIPTING_LUA_REPL_HPP
/* =============================================================================
 * src/scripting/lua/repl.hpp —— 模块侧 REPL 主循环（T7 切片 D1，plan_t7.md §11.7 的 D 行）
 *
 * 职责：把一个已经建好（`create_state_with_libs()`）的 `lua_State` 当交互解释器用：
 *   从 stdin 读一行 → 累积成 chunk → `luaL_loadbuffer` + `lua_pcall` → 用全局 `print`
 *   打结果。**不建状态、不注册 API、不 close**（那三件归调用方 `main.cpp`），
 *   所以 REPL 与脚本路径共用同一份 `lua_State`：`dbg` / `gui` / `open_file` / `help` /
 *   烘焙的 `inspect`/`jsonx`/`moses`/`dbglib` 全都现成可用。
 * 来源：新写（§4.3–4.5 的 REPL 口径 + `AGENTS.md` §3 的退出码口径）。
 * 与旧实现的差异：无（仓库里原先没有 REPL）。
 *
 * 口径（与切片 D1 的任务书逐条对应）：
 *   · **输入**：先 `ReadFile(GetStdHandle(STD_INPUT_HANDLE))`（**管道可跑**，门禁用管道喂
 *     stdin），失败且在真控制台上再回退 `ReadConsoleW`；每次「等可读」都是 ≤100 ms 的切片，
 *     片与片之间 `guard_poll_control()`（无执行连接时近乎零开销）⇒ 不会无限阻塞（S13）。
 *   · **提示符**：只在 stdout 是**真控制台**（`GetConsoleMode` 成功）时打 `autodbg> ` /
 *     `   ...> `；管道/重定向下**零提示符**（否则门禁的输出比对全是噪音）。
 *   · **求值**：`=` 开头的行改写成 `return …`（§11.6 第 1 条）；`luaL_loadbuffer` 报
 *     「在 EOF 处」的语法错 ⇒ 视为**还没输完**，继续读下一行（§11.6 第 2 条）。
 *   · **退出码**：EOF → 返回**最后一条命令**的码（0 = 一路顺利；语法错 3；运行错 2）；
 *     `--repl` 与脚本一样**不改** `AGENTS.md` §3 的口径。
 *   · **本地取消：不提供**（§11.6 第 4 条，已定）：`dbg` 的 `on_wait_timeout` 有意不读
 *     控制台，所以本地 REPL 里长 `pump` 无法 Ctrl+C。这是**决定**，不是 bug。
 *   · **明确不做**（§11.6 结尾）：历史记录 / 箭头键 / Tab 补全（Win9x 控制台 + 零 CRT +
 *     无 readline，代价远大于收益）。
 * ❗`run_repl` 只返回退出码：错误一律打到 stderr（`Error*`），**不** `exit()`（`ExitProcess`
 *   归启动器/`autodbg_run` 的调用方）。
 * ========================================================================== */
#include <lua.hpp>

namespace autodbg::host
{
/* 跑 REPL 主循环直到 stdin EOF；返回退出码（0 = 最后一条命令成功，语法错 3，运行错 2，
 * 读输入失败 -1）。`L` 由调用方持有并负责 `lua_close`。 */
int run_repl(lua_State *L);
} /* namespace autodbg::host */

#endif /* AUTODBG_SCRIPTING_LUA_REPL_HPP */
