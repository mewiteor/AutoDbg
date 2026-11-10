#ifndef AUTODBG_SCRIPTING_LUA_OUTPUT_HPP
#define AUTODBG_SCRIPTING_LUA_OUTPUT_HPP
/* =============================================================================
 * src/scripting/lua/output.hpp —— 一次请求的输出收集器（server 模式）
 *
 * 来源：新写（plan_server.md S1 / §5.3；`print` 覆写按 D30 从 S2 提前到 S1）。
 * 职责：
 *   · 安装 `core::set_output_sink`，把 `Print*` / `Error*` / `log_msg` / `ERR` 的
 *     输出按流（stdout/stderr）收进两条线性缓冲（带上限与截断标记）；
 *   · 覆写 Lua 的全局 `print`（Lua 5.5 与 LuaJIT 都适用），使脚本输出也进同一缓冲
 *     ——否则脚本里最常见的那句 `print("…")` 会跑进 msvcrt 的 stdout 而抓不到；
 *   · 大小上限：每条流超过 `limit_per_stream` 就停止追加、置 `truncated` 并统计丢弃字节。
 * 口径（与 plan_p7_win9x.md §2.4 一致）：缓冲里一律是 **UTF-8 无 BOM**、行尾 `\n`。
 * ❗已知取舍：`print` 的输出**不回显控制台**（只进响应）；`io.write` / `puts` 走真实
 *   stdout（fd 级重定向是 §5.3 的层 B，v1 不做）。这些都记录在 plan_server.md §5.3。
 * ========================================================================== */
#include <stddef.h>

#include <lua.hpp>

namespace autodbg::host
{
struct output_snapshot
{
    const char *out;
    size_t      out_len;
    bool        out_truncated;

    const char *err;
    size_t      err_len;
    bool        err_truncated;

    size_t      dropped_bytes; /* 因上限被丢弃的总字节数 */
};

/* 开始一次收集（清空 + 装 sink）。`limit_per_stream` = 0 → 默认 4 MB。
 * `echo_to_console`：true = `Print*`/`Error*` 同时写控制台（默认），false = 只进缓冲。 */
bool output_begin(size_t limit_per_stream, bool echo_to_console);

/* 结束收集（卸 sink）并返回快照。数据在下一次 `output_begin` 前保持有效。 */
output_snapshot output_end();

/* 是否正在收集（`exec_script` 用它决定要不要覆写 `print`） */
bool output_active();

/* 写一帧到收集器（`stream`：1 = stdout、2 = stderr）；无收集器时静默丢弃 */
void output_write(int stream, const char *data, size_t len);

/* 覆写全局 `print`（只在收集期间安装；语义照抄 Lua 5.5：`tostring` + `\t` + `\n`） */
void install_print_override(lua_State *L);
} /* namespace autodbg::host */

#endif /* AUTODBG_SCRIPTING_LUA_OUTPUT_HPP */
