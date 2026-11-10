#ifndef AUTODBG_SCRIPTING_LUA_EXEC_SESSION_HPP
#define AUTODBG_SCRIPTING_LUA_EXEC_SESSION_HPP
/* =============================================================================
 * src/scripting/lua/exec_session.hpp —— 持久执行会话（S6 / MCP 的会话层）
 *
 * 来源：新写（plan_server.md S6 / §3.4 / §8.3）。
 * 为什么要会话：无状态的 `/run` 每次新建 `lua_State` + `DebuggerSession`，
 *   于是「设断点 → 再 run → 再读寄存器」这种交互式用法无法表达。会话把这两样
 *   **跨请求保留**：全局变量、断点、被调试进程都活着。
 * 约束：
 *   · 会话只在 **worker 线程**（T2）上创建/使用/销毁 —— 执行严格串行，不加锁；
 *   · 上限 `k_max_sessions` 个；空闲超过 `k_session_idle_ms` 由 `session_sweep` 回收；
 *   · id 是自增十进制数（同时用作 `Mcp-Session-Id`，§8.3）。
 * ========================================================================== */
#include <windows.h>
#include <stddef.h>

#include "exec.hpp"

namespace autodbg::core
{
class DebuggerSession;
}

namespace autodbg::host
{
constexpr unsigned k_max_sessions    = 8;
constexpr DWORD    k_session_idle_ms = 30u * 60u * 1000u;

struct exec_session
{
    unsigned               id = 0;
    DWORD                  last_used = 0;
    unsigned               requests = 0;
    lua_State             *L = nullptr;
    core::DebuggerSession *dbg = nullptr;

    /* S7：client 文件 I/O 的挂起点（同一时刻最多一个：执行是串行的） */
    bool                   pending_active = false;
    int                    pending_co_ref  = -1; /* 挂起的协程（session->L 的 registry ref） */
    unsigned               pending_gen    = 0;  /* token 的后半段（防陈旧 resume */
    bool                   pending_is_mcp = false;
    char                   pending_op[24] = {}; /* read_file / write_file */

    bool valid() const noexcept { return id != 0 && L != nullptr; }
};

/* 建会话（失败返回 nullptr，err 里是原因） */
exec_session *session_create(char *err, size_t err_cap);

/* 按数字 id 查找（`id == 0` 或不存在 → nullptr） */
exec_session *session_find(unsigned id);

/* 从 `Mcp-Session-Id` 的**文本**解析并查找（非数字/不存在 → nullptr） */
exec_session *session_find_by_text(const char *id_text);

/* 关闭并释放（不存在返回 false） */
bool session_close(unsigned id);

unsigned session_count();

/* 回收空闲会话：返回回收个数（worker 每次出队时顺手调一次即可） */
unsigned session_sweep(DWORD now);

/* 在会话里执行（保留状态）；返回结果 */
exec_result session_exec(exec_session *s, const exec_request &req);

/* 调会话里的 `mcp.dispatch(body)`（S6 的 MCP 入口）。
 * 成功且返回非空时 `*out` 是新分配的 JSON 串（用 `common::release` 释放）；
 * 返回 true 而 `*out == nullptr` = 这是一条通知（不需要响应）。 */
bool session_mcp_dispatch(exec_session *s, const char *body, size_t len,
                          char **out, size_t *out_len, char *err, size_t err_cap);

/* ---- S7：client 文件 I/O 的挂起/恢复 ---- */
/* 记录挂起点（op 为 read_file/write_file）；失败返回 false */
bool session_set_pending(exec_session *s, int co_ref, const char *op, bool is_mcp);
void session_clear_pending(exec_session *s);

/* 按 `"<sid>:<gen>"` 形式的 token 找会话并校验代次（陈旧/不存在 → nullptr） */
exec_session *session_by_token(const char *token, unsigned *gen_out);

/* 挂起点的字段读取（给 server 组响应 / 调 effects.unpack 用） */
const char *session_pending_op(exec_session *s);
int         session_pending_co_ref(exec_session *s);
bool        session_pending_is_mcp(exec_session *s);
unsigned    session_pending_gen(exec_session *s);

/* 进程收尾：关掉全部会话（server_main 退出时调用） */
void session_close_all();
} /* namespace autodbg::host */

#endif /* AUTODBG_SCRIPTING_LUA_EXEC_SESSION_HPP */
