/* =============================================================================
 * src/scripting/lua/exec.cpp —— 执行器实现（S0 基础 + S6 拆出 execute-on-state）
 *
 * 来源：S0 新写；S6 把「状态/会话的生命周期」与「执行」拆开，让持久会话能复用。
 * 与旧实现的差异：无（新文件）；会话/状态的生命周期与 `main.cpp::_tmain` 同口径，
 *   状态构建复用 `state.cpp::create_state_with_libs()`。
 * 口径：
 *   · `g_wfilepath` 在执行期间指到「客户端文件名」（UNICODE 构建转 UTF-16），退出时恢复；
 *   · `chunkname` 直接交给 `luaL_loadbuffer`：Lua 会把错误写成 `@buffer.lua:12: …`；
 *   · 失败时把栈顶错误串拷进 `exec_result::error`（UTF-8，截断到 1023 字节）。
 * ========================================================================== */
#include "exec.hpp"

#include "exec_guard.hpp" /* S2：取消/超时守卫（Lua 计数钩子） */
#include "output.hpp"     /* S1：收集器 + print 覆写（D30） */
#include "state.hpp"      /* create_state_with_libs / g_wfilepath */

#include "core/core.hpp"  /* DebuggerSession / set_current_session */
#include "core/types.hpp" /* MAX_FILEPATH / TCHAR */
#include "core/util.hpp"  /* utf8_to_utf16_n（仅 _UNICODE）/ ErrorS */

#include <string.h>

namespace
{
/* 把 Lua 栈顶的错误对象拷进 out->error（不 pop） */
void capture_error(lua_State *L, exec_result *out)
{
    out->error[0] = '\0';
    size_t      n   = 0;
    const char *err = lua_tolstring(L, -1, &n);
    if (err == nullptr)
    {
        const char *fallback = "(non-string error object)";
        size_t      fl       = strlen(fallback);
        if (fl > sizeof out->error - 1)
            fl = sizeof out->error - 1;
        memcpy(out->error, fallback, fl);
        out->error[fl] = '\0';
        return;
    }
    if (n > sizeof out->error - 1)
        n = sizeof out->error - 1;
    memcpy(out->error, err, n);
    out->error[n] = '\0';
}

/* 把 `g_wfilepath` 临时指到 chunk 名（去掉 `@`）；返回原值以便恢复 */
LPCTSTR set_temp_path(const char *name, LPWSTR *wbuf_out)
{
    LPCTSTR     saved     = g_wfilepath;
    const char *file_only = (name[0] == '@') ? (name + 1) : name;
    if (wbuf_out != nullptr)
        *wbuf_out = nullptr;
#ifdef _UNICODE
    int    wn    = (int)strlen(file_only);
    LPWSTR wfile = autodbg::core::utf8_to_utf16_n(file_only, &wn);
    if (wfile != nullptr)
    {
        g_wfilepath = wfile;
        if (wbuf_out != nullptr)
            *wbuf_out = wfile;
    }
#else
    (void)wbuf_out;
    g_wfilepath = file_only;
#endif
    return saved;
}
} /* namespace */

exec_result exec_on_state(lua_State *L, autodbg::core::DebuggerSession *dbg, const exec_request &req)
{
    exec_result out;
    out.code       = -1;
    out.loaded     = false;
    out.oom        = false;
    out.elapsed_ms = 0;
    out.error[0]   = '\0';

    if (L == nullptr)
    {
        out.oom = true;
        return out;
    }
    if (req.src == nullptr || req.len == 0)
    {
        const char *msg = "empty script";
        size_t      n   = strlen(msg);
        memcpy(out.error, msg, n);
        out.error[n] = '\0';
        return out;
    }

    /* 会话必须在任何引擎调用之前挂上（core 的 API 用 current_session()） */
    autodbg::core::set_current_session(dbg);

    const char *name = (req.chunkname != nullptr) ? req.chunkname : "@<server>";

    LPWSTR  wfile     = nullptr;
    LPCTSTR saved     = set_temp_path(name, &wfile);

    DWORD t0 = GetTickCount();

    int r = luaL_loadbuffer(L, req.src, req.len, name);
    out.code   = r;
    out.loaded = (r == 0);

    if (r == 0)
    {
        int argc = (req.args != nullptr) ? req.argc : 0;
        for (int i = 0; i < argc; i++)
            lua_pushstring(L, (req.args[i] != nullptr) ? req.args[i] : "");
        r = lua_pcall(L, argc, LUA_MULTRET, 0);
        out.code = r;
    }

    if (r != 0)
    {
        capture_error(L, &out);
        /* 清掉错误对象（持久会话要继续用这个栈） */
        lua_pop(L, 1);
    }
    else
    {
        /* 顶层返回值也弹掉：无状态路径根本不用；持久会话也不该让栈无限长 */
        lua_settop(L, 0);
    }

    out.elapsed_ms = GetTickCount() - t0;

    g_wfilepath = saved;
#ifdef _UNICODE
    if (wfile != nullptr)
        autodbg::common::release(wfile);
#endif

    autodbg::core::set_current_session(nullptr);
    return out;
}

/* ============================ 协程执行（S7） ============================ */
namespace
{
/* 两个 Lua 版本的 `lua_resume` 原型不同（见 exec.hpp 的说明） */
int resume_co(lua_State *co, int nargs)
{
#ifdef JIT
    return lua_resume(co, nargs);
#else
    int nres = 0;
    return lua_resume(co, nullptr, nargs, &nres);
#endif
}

void init_run(exec_run *out)
{
    out->suspended            = false;
    out->co_ref               = -1;
    out->result.code          = -1;
    out->result.loaded        = false;
    out->result.oom           = false;
    out->result.elapsed_ms    = 0;
    out->result.error[0]      = '\\0';
    out->ret                  = nullptr;
    out->ret_len              = 0;
}

void set_msg(char *dst, size_t cap, const char *msg)
{
    if (dst == nullptr || cap == 0)
        return;
    size_t n = (msg != nullptr) ? strlen(msg) : 0;
    if (n > cap - 1)
        n = cap - 1;
    if (n > 0)
        memcpy(dst, msg, n);
    dst[n] = '\0';
}

/* 协程返回值是字符串时拷一份（/mcp 靠它拿 JSON-RPC 响应） */
void take_return(lua_State *co, exec_run *out)
{
    if (lua_gettop(co) < 1 || lua_type(co, -1) != LUA_TSTRING)
        return;
    size_t      n = 0;
    const char *s = lua_tolstring(co, -1, &n);
    if (s == nullptr)
        return;
    char *buf = (char *)autodbg::common::alloc(n + 1);
    if (buf == nullptr)
        return;
    memcpy(buf, s, n);
    buf[n]      = '\\0';
    out->ret     = buf;
    out->ret_len = n;
}
} /* namespace */

exec_run exec_begin(lua_State *L, autodbg::core::DebuggerSession *dbg, const exec_request &req)
{
    exec_run out;
    init_run(&out);

    if (L == nullptr)
    {
        out.result.oom = true;
        return out;
    }
    if (req.src == nullptr || req.len == 0)
    {
        set_msg(out.result.error, sizeof out.result.error, "empty script");
        return out;
    }

    autodbg::core::set_current_session(dbg);

    const char *name = (req.chunkname != nullptr) ? req.chunkname : "@<server>";

    LPWSTR  wfile = nullptr;
    LPCTSTR saved = set_temp_path(name, &wfile);
    DWORD   t0    = GetTickCount();

    int  base = lua_gettop(L);
    int  r    = luaL_loadbuffer(L, req.src, req.len, name);
    out.result.code   = r;
    out.result.loaded = (r == 0);

    if (r != 0)
    {
        capture_error(L, &out.result);
    }
    else
    {
        /* chunk 装进新协程：栈 [chunk] → [chunk][thread] → 把 chunk 搬到协程上 */
        lua_State *co = lua_newthread(L);
        lua_pushvalue(L, -2);
        lua_xmove(L, co, 1);
        int co_ref = luaL_ref(L, LUA_REGISTRYINDEX); /* 弹掉 thread */

        /* 计数钩子按线程生效：新协程要单独装（取消/超时，S2） */
        autodbg::host::guard_install_lua_hook(co);

        int argc = (req.args != nullptr) ? req.argc : 0;
        for (int i = 0; i < argc; i++)
            lua_pushstring(co, (req.args[i] != nullptr) ? req.args[i] : "");

        int rr = resume_co(co, argc);
        if (rr == LUA_YIELD)
        {
            out.suspended   = true;
            out.co_ref      = co_ref;
            out.result.code = -2; /* 哨兵：挂起 */
        }
        else if (rr == LUA_OK)
        {
            take_return(co, &out);
            luaL_unref(L, LUA_REGISTRYINDEX, co_ref);
            out.result.code = 0;
        }
        else
        {
            out.result.code = rr; /* 错误码必须落进结果（否则会被当成成功） */
            capture_error(co, &out.result);
            luaL_unref(L, LUA_REGISTRYINDEX, co_ref);
        }
    }

    lua_settop(L, base);
    out.result.elapsed_ms = GetTickCount() - t0;

    g_wfilepath = saved;
#ifdef _UNICODE
    if (wfile != nullptr)
        autodbg::common::release(wfile);
#endif

    autodbg::core::set_current_session(nullptr);
    return out;
}

bool exec_push_results(lua_State *L, int co_ref, int n)
{
    if (L == nullptr || co_ref < 0 || n <= 0)
        return false;
    lua_rawgeti(L, LUA_REGISTRYINDEX, co_ref);
    if (!lua_isthread(L, -1))
    {
        lua_pop(L, 1);
        return false;
    }
    lua_State *co = lua_tothread(L, -1);
    lua_pop(L, 1);
    lua_xmove(L, co, n); /* L 栈顶 n 个 → 协程栈 */
    return true;
}

exec_run exec_resume(lua_State *L, autodbg::core::DebuggerSession *dbg, int co_ref, int nargs)
{
    exec_run out;
    init_run(&out);

    if (L == nullptr || co_ref < 0)
    {
        set_msg(out.result.error, sizeof out.result.error, "invalid coroutine");
        return out;
    }

    lua_rawgeti(L, LUA_REGISTRYINDEX, co_ref);
    if (!lua_isthread(L, -1))
    {
        lua_pop(L, 1);
        set_msg(out.result.error, sizeof out.result.error, "invalid coroutine");
        return out;
    }
    lua_State *co = lua_tothread(L, -1);
    lua_pop(L, 1);

    autodbg::core::set_current_session(dbg);
    DWORD t0 = GetTickCount();

    int rr = resume_co(co, nargs);
    if (rr == LUA_YIELD)
    {
        out.suspended   = true;
        out.co_ref      = co_ref;
        out.result.code = -2;
    }
    else if (rr == LUA_OK)
    {
        take_return(co, &out);
        luaL_unref(L, LUA_REGISTRYINDEX, co_ref);
        out.result.code   = 0;
        out.result.loaded = true;
    }
    else
    {
        out.result.code = rr; /* 错误码必须落进结果（否则会被当成成功） */
        capture_error(co, &out.result);
        out.result.loaded = true;
        luaL_unref(L, LUA_REGISTRYINDEX, co_ref);
    }

    out.result.elapsed_ms = GetTickCount() - t0;
    autodbg::core::set_current_session(nullptr);
    return out;
}

void exec_drop(lua_State *L, int co_ref)
{
    if (L != nullptr && co_ref >= 0)
        luaL_unref(L, LUA_REGISTRYINDEX, co_ref);
}
exec_result exec_script(const exec_request &req)
{
    exec_result out;
    out.code       = -1;
    out.loaded     = false;
    out.oom        = false;
    out.elapsed_ms = 0;
    out.error[0]   = '\0';

    /* ---- 会话（与 _tmain 同口径，§5.2） ---- */
    autodbg::core::DebuggerSession *session = new autodbg::core::DebuggerSession();

    DWORD t0 = GetTickCount();
    lua_State *L = create_state_with_libs();
    if (L == nullptr)
    {
        out.oom  = true;
        out.code = -1;
        autodbg::core::ErrorS("exec: luaL_newstate failed\n");
    }
    else
    {
        if (autodbg::host::output_active())
        {
            /* server 模式：`print` 要进响应；纯 Lua 死循环靠计数钩子插话 */
            autodbg::host::install_print_override(L);
            autodbg::host::guard_install_lua_hook(L);
        }

        out = exec_on_state(L, session, req);
        lua_close(L);
    }
    out.elapsed_ms = GetTickCount() - t0;

    delete session;
    autodbg::core::set_current_session(nullptr);
    return out;
}
