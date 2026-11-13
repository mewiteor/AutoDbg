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
    /* T7 切片 C-remote（D56）：顶层 chunk 的返回值，口径与 `--json` 信封的 `result` 完全一致
     * （`state.hpp` 的 `encode_returns`：1 个 → 那个值；0 个 → `null`；多个 → 数组；编不了 → `null`）。
     * 它进协议 `result` 帧的 `json.value`（远端会话代理靠它把方法返回值取回来）。
     * ❗所有权：`common::alloc` 的缓冲区，**调用方 `common::release`**；nullptr = 没有返回值
     * （加载/语法/运行失败，或宿主失败）—— 发帧那边就写 `null`。`exec_result` 是按值拷的，
     * 但只有一个持有者。 */
    const char *value_json;
};

/* 在给定的状态与会话上执行（调用方负责状态/会话的生命周期）。
 * `dbg` 非空时会临时挂给 `core::set_current_session()`（执行完恢复成 nullptr）。 */
exec_result exec_on_state(lua_State *L, autodbg::core::DebuggerSession *dbg, const exec_request &req);

/* 一次性执行：建会话 + 建状态（含烘焙库）→ 执行 → 全部释放 */
exec_result exec_script(const exec_request &req);

#endif /* AUTODBG_SCRIPTING_LUA_EXEC_HPP */
