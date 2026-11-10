/* =============================================================================
 * src/scripting/lua/exec_session.cpp —— 持久执行会话实现（S6）
 *
 * 来源：新写（plan_server.md S6）。
 * 与旧实现的差异：无（新文件）。
 * 实现要点：
 *   ① 会话表是**模块级静态数组**（不是函数内静态：那会引入线程安全初始化 →
 *      `_tls_index` → 手工映射模块 LNK2001，S2/S4 已各踩一次）；
 *   ② 每个会话持有一个 `lua_State`（烘焙库 + 注册表 + `print` 覆写 + 计数钩子）
 *      和一个 `DebuggerSession`（断点/模块/被调试进程）；
 *   ③ 执行走 `exec_on_state()`，与无状态路径共用同一套语义。
 * ========================================================================== */
#include "exec_session.hpp"

#include "exec_guard.hpp"
#include "output.hpp"
#include "state.hpp"

#include "core/core.hpp"
#include "core/util.hpp"

#include <string.h>

namespace autodbg::host
{
namespace
{
exec_session g_sessions[k_max_sessions];
unsigned     g_next_id = 0;

/* 找空闲槽（id == 0） */
exec_session *find_slot()
{
    for (unsigned i = 0; i < k_max_sessions; i++)
    {
        if (g_sessions[i].id == 0)
            return &g_sessions[i];
    }
    return nullptr;
}

void destroy(exec_session *s)
{
    if (s == nullptr)
        return;
    session_clear_pending(s); /* S7：先放掉挂起的协程 */
    if (s->L != nullptr)
    {
        lua_close(s->L);
        s->L = nullptr;
    }
    if (s->dbg != nullptr)
    {
        delete s->dbg;
        s->dbg = nullptr;
    }
    s->id        = 0;
    s->last_used = 0;
    s->requests  = 0;
}
} /* namespace */

exec_session *session_create(char *err, size_t err_cap)
{
    if (err != nullptr && err_cap > 0)
        err[0] = '\0';

    exec_session *s = find_slot();
    if (s == nullptr)
    {
        if (err != nullptr && err_cap > 0)
        {
            const char *msg = "too many sessions";
            size_t      n   = strlen(msg);
            if (n > err_cap - 1)
                n = err_cap - 1;
            memcpy(err, msg, n);
            err[n] = '\0';
        }
        return nullptr;
    }

    lua_State *L = create_state_with_libs();
    if (L == nullptr)
    {
        if (err != nullptr && err_cap > 0)
        {
            const char *msg = "luaL_newstate failed";
            size_t      n   = strlen(msg);
            if (n > err_cap - 1)
                n = err_cap - 1;
            memcpy(err, msg, n);
            err[n] = '\0';
        }
        return nullptr;
    }

    /* 会话里的输出/取消语义与无状态路径一致（D30 / D32） */
    install_print_override(L);
    guard_install_lua_hook(L);

    s->id        = ++g_next_id;
    s->L         = L;
    s->dbg       = new core::DebuggerSession();
    s->last_used = GetTickCount();
    s->requests  = 0;
    return s;
}

exec_session *session_find(unsigned id)
{
    if (id == 0)
        return nullptr;
    for (unsigned i = 0; i < k_max_sessions; i++)
    {
        if (g_sessions[i].id == id)
            return &g_sessions[i];
    }
    return nullptr;
}

exec_session *session_find_by_text(const char *id_text)
{
    if (id_text == nullptr || id_text[0] == '\0')
        return nullptr;
    unsigned v = 0;
    for (const char *p = id_text; *p != '\0'; p++)
    {
        if (*p < '0' || *p > '9')
            return nullptr;
        v = v * 10 + (unsigned)(*p - '0');
        if (v > 0xFFFFFFu)
            return nullptr;
    }
    return session_find(v);
}

bool session_close(unsigned id)
{
    exec_session *s = session_find(id);
    if (s == nullptr)
        return false;
    destroy(s);
    return true;
}

unsigned session_count()
{
    unsigned n = 0;
    for (unsigned i = 0; i < k_max_sessions; i++)
    {
        if (g_sessions[i].id != 0)
            n++;
    }
    return n;
}

unsigned session_sweep(DWORD now)
{
    unsigned closed = 0;
    for (unsigned i = 0; i < k_max_sessions; i++)
    {
        exec_session *s = &g_sessions[i];
        if (s->id == 0)
            continue;
        if ((DWORD)(now - s->last_used) >= k_session_idle_ms)
        {
            destroy(s);
            closed++;
        }
    }
    return closed;
}

exec_result session_exec(exec_session *s, const exec_request &req)
{
    exec_result out;
    out.code       = -1;
    out.loaded     = false;
    out.oom        = false;
    out.elapsed_ms = 0;
    out.error[0]   = '\0';

    if (s == nullptr || !s->valid())
    {
        const char *msg = "invalid session";
        size_t      n   = strlen(msg);
        memcpy(out.error, msg, n);
        out.error[n] = '\0';
        return out;
    }

    s->last_used = GetTickCount();
    s->requests++;

    return exec_on_state(s->L, s->dbg, req);
}

void session_close_all()
{
    for (unsigned i = 0; i < k_max_sessions; i++)
        destroy(&g_sessions[i]);
}

namespace
{
void set_err_text(char *err, size_t cap, const char *text)
{
    if (err == nullptr || cap == 0)
        return;
    size_t n = (text != nullptr) ? strlen(text) : 0;
    if (n > cap - 1)
        n = cap - 1;
    if (n > 0)
        memcpy(err, text, n);
    err[n] = '\0';
}
} /* namespace */

bool session_mcp_dispatch(exec_session *s, const char *body, size_t len,
                          char **out, size_t *out_len, char *err, size_t err_cap)
{
    if (out != nullptr)
        *out = nullptr;
    if (out_len != nullptr)
        *out_len = 0;
    if (err != nullptr && err_cap > 0)
        err[0] = '\0';

    if (s == nullptr || !s->valid())
    {
        set_err_text(err, err_cap, "invalid session");
        return false;
    }

    lua_State *L = s->L;
    core::set_current_session(s->dbg);

    int  base = lua_gettop(L);
    bool ok   = false;

    lua_getglobal(L, "mcp");
    if (!lua_istable(L, -1))
    {
        set_err_text(err, err_cap, "mcp library not baked");
        lua_settop(L, base);
        core::set_current_session(nullptr);
        return false;
    }
    lua_getfield(L, -1, "dispatch");
    if (!lua_isfunction(L, -1))
    {
        set_err_text(err, err_cap, "mcp.dispatch missing");
        lua_settop(L, base);
        core::set_current_session(nullptr);
        return false;
    }

    lua_pushlstring(L, body, len);
    if (lua_pcall(L, 1, 1, 0) != 0)
    {
        const char *msg = lua_tostring(L, -1);
        set_err_text(err, err_cap, (msg != nullptr) ? msg : "mcp.dispatch error");
    }
    else if (lua_isnil(L, -1))
    {
        ok = true; /* 通知：不需要响应 */
    }
    else
    {
        size_t      n = 0;
        const char *r = lua_tolstring(L, -1, &n);
        if (r != nullptr)
        {
            char *buf = (char *)common::alloc(n + 1);
            if (buf != nullptr)
            {
                memcpy(buf, r, n);
                buf[n] = '\0';
                if (out != nullptr)
                    *out = buf;
                if (out_len != nullptr)
                    *out_len = n;
                ok = true;
            }
            else
            {
                set_err_text(err, err_cap, "oom");
            }
        }
    }

    lua_settop(L, base);
    s->last_used = GetTickCount();
    core::set_current_session(nullptr);
    return ok;
}

/* ============================ S7：挂起 / 恢复（client 文件 I/O） ============================ */
bool session_set_pending(exec_session *s, int co_ref, const char *op, bool is_mcp)
{
    if (s == nullptr || !s->valid())
        return false;
    /* 串行执行 → 不可能并存；但要允许「同一个协程继续挂起」（S7 的连续 effect）：
     * 那时 co_ref 相同，**不能** unref（否则引用槽被回收，协程可能已被 GC）。 */
    if (s->pending_active && s->pending_co_ref != co_ref)
        session_clear_pending(s);

    s->pending_active = true;
    s->pending_co_ref = co_ref;
    s->pending_gen++;
    s->pending_is_mcp = is_mcp;

    size_t n = (op != nullptr) ? strlen(op) : 0;
    if (n + 1 > sizeof s->pending_op)
        n = sizeof s->pending_op - 1;
    if (n > 0)
        memcpy(s->pending_op, op, n);
    s->pending_op[n] = '\0';

    s->last_used = GetTickCount();
    return true;
}

void session_clear_pending(exec_session *s)
{
    if (s == nullptr)
        return;
    if (s->pending_active && s->pending_co_ref >= 0 && s->L != nullptr)
        exec_drop(s->L, s->pending_co_ref);
    s->pending_active = false;
    s->pending_co_ref = -1;
    s->pending_op[0]  = '\0';
    s->pending_is_mcp = false;
}

exec_session *session_by_token(const char *token, unsigned *gen_out)
{
    if (token == nullptr || token[0] == '\0')
        return nullptr;

    const char *colon = nullptr;
    for (const char *p = token; *p != '\0'; p++)
    {
        if (*p == ':')
        {
            colon = p;
            break;
        }
    }
    if (colon == nullptr || colon == token)
        return nullptr;

    unsigned sid = 0;
    for (const char *p = token; p < colon; p++)
    {
        if (*p < '0' || *p > '9')
            return nullptr;
        sid = sid * 10 + (unsigned)(*p - '0');
        if (sid > 1000000u)
            return nullptr;
    }

    unsigned gen = 0;
    for (const char *p = colon + 1; *p != '\0'; p++)
    {
        if (*p < '0' || *p > '9')
            return nullptr;
        gen = gen * 10 + (unsigned)(*p - '0');
        if (gen > 1000000u)
            return nullptr;
    }

    exec_session *s = session_find(sid);
    if (s == nullptr || !s->pending_active || s->pending_gen != gen)
        return nullptr;

    if (gen_out != nullptr)
        *gen_out = gen;
    return s;
}

const char *session_pending_op(exec_session *s)
{
    return (s != nullptr) ? s->pending_op : "";
}

int session_pending_co_ref(exec_session *s)
{
    return (s != nullptr) ? s->pending_co_ref : -1;
}

bool session_pending_is_mcp(exec_session *s)
{
    return (s != nullptr) && s->pending_is_mcp;
}

unsigned session_pending_gen(exec_session *s)
{
    return (s != nullptr) ? s->pending_gen : 0u;
}
} /* namespace autodbg::host */
