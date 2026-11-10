#ifndef AUTODBG_SCRIPTING_LUA_EXEC_HPP
#define AUTODBG_SCRIPTING_LUA_EXEC_HPP
/* =============================================================================
 * src/scripting/lua/exec.hpp —— 「执行一段内存里的 Lua 源码」
 *
 * 来源：新写（S0 基础 + S6 拆出 execute-on-state）。
 * 职责：
 *   · `exec_on_state()`：在**已存在的** `lua_State` + `DebuggerSession` 上执行一段源码
 *     （S6 的持久会话用它；无状态 `/run` 也复用）；
 *   · `exec_script()`：一次性语义——建状态与会话 → 执行 → 全部释放（D22 的 v1 行为）。
 * 约束：不写任何临时文件（模块不再往脚本目录写 `cdebugger.d.lua` / `.luarc.json`：
 *   编辑器提示改由 neovim 插件的 LuaLS 注解库提供）；不抛异常；
 *   错误以返回值 + `error[]` 文本表达。
 * ========================================================================== */
#include <stddef.h>

#include <lua.hpp>

namespace autodbg::core
{
class DebuggerSession;
}

/* 一次执行请求 */
struct exec_request
{
    const char        *src;       /* Lua 源码（UTF-8；不要求 NUL 结尾） */
    size_t             len;
    const char        *chunkname; /* 建议 `@<客户端文件名>`；nullptr → `@<server>` */
    const char *const *args;      /* 主 chunk 的 vararg（UTF-8） */
    int                argc;
};

/* 一次执行结果 */
struct exec_result
{
    int      code;   /* 0 = 成功；>0 = Lua 错误码；-1 = 宿主侧失败 */
    bool     loaded; /* false = 加载/语法阶段失败 */
    bool     oom;
    unsigned elapsed_ms;
    char     error[1024]; /* UTF-8；成功时为空串 */
};

/* 在给定的状态与会话上执行（调用方负责状态/会话的生命周期）。
 * `dbg` 非空时会临时挂给 `core::set_current_session()`（执行完恢复成 nullptr）。 */
exec_result exec_on_state(lua_State *L, autodbg::core::DebuggerSession *dbg, const exec_request &req);

/* ============================ 协程执行（S7：client 文件 I/O） ============================
 * `read_file`/`write_file` 需要把「文件去客户端读/写」这件事交给宿主： Lua 函数
 * `lua_yield` 一个效果表，宿主把它发给客户端，拿到结果后再 `resume`。
 * 因此一次远程执行可能是**多段**的：begin → (yield) → resume → … → 完成。
 * ❗两个 Lua 版本的 `lua_resume` 原型不同（LuaJIT: `(L, narg)`；Lua 5.5: `(L, from, narg, &nres)`）
 *   → 用 `#ifdef JIT` 分支（构建时 luajit flavor 会定义 JIT）。 */
struct exec_run
{
    exec_result result;    /* 完成/失败时有效；suspended 时 `code = -2` 作为哨兵 */
    bool        suspended; /* true = 协程停在 `lua_yield` 上 */
    int         co_ref;    /* 未取消的协程引用（LUA_REGISTRYINDEX）；完成/失败时已 unref → -1 */
    char       *ret;       /* 完成时：若协程返回值是字符串，拷一份出来（caller 用 release 释放） */
    size_t      ret_len;
};

/* 启动协程执行；suspended 时协程里 yield 出来的值仍在协程栈上（宿主用 `effect_*` 读） */
exec_run exec_begin(lua_State *L, autodbg::core::DebuggerSession *dbg, const exec_request &req);

/* 把 `L` 栈顶的 `n` 个值搬到挂起的协程上（resume 的实参/返回値） */
bool exec_push_results(lua_State *L, int co_ref, int n);

/* 继续执行：
args = 已经压在协程栈上的结果值个数（xec_push_results 压的） */
exec_run exec_resume(lua_State *L, autodbg::core::DebuggerSession *dbg, int co_ref, int nargs);

/* 丢弃一个挂起的协程（unref；会话关闭/超时回收时用） */
void exec_drop(lua_State *L, int co_ref);

/* 一次性执行：建会话 + 建状态（含烘焙库）→ 执行 → 全部释放 */
exec_result exec_script(const exec_request &req);

#endif /* AUTODBG_SCRIPTING_LUA_EXEC_HPP */
