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
