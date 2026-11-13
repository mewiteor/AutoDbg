#ifndef AUTODBG_SCRIPTING_LUA_REPL_HPP
#define AUTODBG_SCRIPTING_LUA_REPL_HPP
/* =============================================================================
 * src/scripting/lua/repl.hpp —— 模块侧 REPL（T7 切片 D1 的主循环 + D2b 的服务端形态）
 *
 * 职责：把一个已经建好（`create_state_with_libs()`）的 `lua_State` 当交互解释器用。
 *   **不建状态、不注册 API、不 close**（那三件归调用方 `main.cpp` / `tcp_server.cpp`），
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
 *
 * 切片 D2b（本文件的下半部分）：
 *   · 求值拆成「累积 chunk」（`repl_chunk_*`）与「求值 chunk」（`repl_eval_chunk`）两半，
 *     **本地循环与服务端共用同一份** `=` 简写 / 续行判定 / 错误码口径；
 *   · `--repl --server`：`repl_server_begin/chunk/end` 让 `lua_State` 与引擎会话在**一条连接**
 *     的生命周期内不重建，每次 `script` 帧 = 一行（或一个完整块）；
 *   · 两条路径退出前都做 D54 的安全网（`repl_unclosed_session_report`）：没关的会话 detach
 *     （没有该能力的平台退 kill），报告只进 stderr，**不改退出码**。
 * ❗`run_repl` 只返回退出码：错误一律打到 stderr（`Error*`），**不** `exit()`（`ExitProcess`
 *   归启动器/`autodbg_run` 的调用方）。
 * ========================================================================== */
#include <stddef.h>

#include <lua.hpp>

#include "exec.hpp" /* exec_result：`repl_server_chunk` 的形状与脚本路径相同（`result` 帧直接复用） */

namespace autodbg::core
{
class DebuggerSession;
}

namespace autodbg::host
{
/* 一个 REPL chunk 的上限（4 KB）：REPL 里超长行没有意义，超了就截断（Lua 会报语法错）。 */
constexpr unsigned k_repl_chunk_cap = 4096;

/* 累积中的 chunk（`\n` 分隔的多行）。本地循环跨行用它，服务端每帧用它。 */
struct repl_chunk
{
    char     buf[k_repl_chunk_cap];
    unsigned len;
};

void repl_chunk_reset(repl_chunk *c);

/* 把一行输入并进 chunk，返回 false = 超过上限（调用方按语法错处理）。
 * `=` 简写（只在 chunk 为空时生效）实现在这里 —— 本地与服务端因此逐字节同口径。 */
bool repl_chunk_feed(repl_chunk *c, const char *text, unsigned len);

/* 求值一次累积的 chunk。返回 1 = 还没输完（`allow_continue` 为真且 Lua 报「在 <eof> 处」），
 * 0 = 已处理（成功或出错；出错时已经把消息打到 stderr，并把 Lua 的码写进 `*last_code`）。
 * `allow_continue = false`：调用方已经没有下一行（本地 EOF / 服务端一帧一命令）⇒「没输完」
 * 按语法错上报（3），而不是等一条永远不会来的输入。
 * `value_json`（可空）：非空且成功时写入该 chunk 返回值的 JSON 片段（`state.hpp` 的
 * `encode_returns` 口径，`common::alloc` 的缓冲区、**调用方 release**）—— T7 C-remote（D56）
 * 靠它把 `dbglib.remote_call` 的返回值送进 `result` 帧的 `json.value`；本地 REPL 传 nullptr，
 * 于是它的行为一个字节不改（返回值仍然只是 `print` 出来）。 */
int repl_eval_chunk(lua_State *L, const char *chunk, size_t len, bool allow_continue, int *last_code,
                    const char **value_json = nullptr);

/* 跑本地 REPL 主循环直到 stdin EOF；返回退出码（0 = 最后一条命令成功，语法错 3，运行错 2，
 * 读输入失败 -1）。`L` 由调用方持有并负责 `lua_close`。 */
int run_repl(lua_State *L);

/* ---- `--repl --server`（T7 切片 D2b，plan_t7.md §11.7 的 D2b 行） ----
 * 语义：`--repl` 下 server 的 `lua_State` 与引擎会话**在一条连接的生命周期内不重建**；
 * 每次 `script` 帧 = 一行（或一个完整块），在该 state 上求值；`out` 帧的 `id` = 命令号
 * （D2a 已有），每条命令末尾一帧 `result`（`id` = 这一行的 Lua 错误码） + 一帧 `end`。
 * 非 `--repl` 时一个字节都不走这里（`exec_script` 仍然是每次 `script` 建/毁 state，D43）。 */
struct repl_server
{
    lua_State                      *L;    /* 连接级 Lua 状态（`lua_close` 归 `repl_server_end`） */
    autodbg::core::DebuggerSession *sess; /* 连接级引擎会话（D1 口径：与脚本路径同一份） */
};

/* 建连接级的 state + 会话（`print` 覆写 / 计数钩子也在这里装一次）。失败 → nullptr。 */
repl_server *repl_server_begin();

/* 求值**一帧**（一行 / 一个完整块）：不做跨帧的续行累积，错误码与错误消息口径同本地 REPL。 */
exec_result repl_server_chunk(repl_server *r, const char *src, size_t len);

/* 收尾：会话报告（D54 安全网）→ `lua_close` → 释放引擎会话。 */
void repl_server_end(repl_server *r);
} /* namespace autodbg::host */

#endif /* AUTODBG_SCRIPTING_LUA_REPL_HPP */
