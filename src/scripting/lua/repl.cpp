/* =============================================================================
 * src/scripting/lua/repl.cpp —— 模块侧 REPL（T7 切片 D1 主循环 + D2b 服务端形态）
 *
 * 来源：新写（§4.3–4.5 的口径 + §11.6 的四个「对人的缺口」）。仓库原先没有 REPL。
 * 与旧实现的差异：无。
 *
 * 形状（本地三段：读 → 求值 → 打结果）：
 *   ① `repl_input` 把 stdin 的字节流切成行：先 `ReadFile`（**管道可跑**，门禁靠它），
 *      失败且是真控制台时回退 `ReadConsoleW`；每次「等可读」都是 ≤100 ms 的切片，片间
 *      `guard_poll_control()`（S13：REPL 的输入不能无限阻塞）。
 *   ② `repl_chunk_feed` + `repl_eval_chunk` 把累积的 chunk 交给 `luaL_loadbuffer(L, …, "=repl")`
 *      + `lua_pcall`：「在 EOF 处」的语法错 = 还没输完 ⇒ 继续读行（多行续行）；其它语法错 /
 *      运行错 ⇒ 打到 stderr 并把退出码记成 Lua 的码（3 / 2）。
 *   ③ 结果用**全局 `print`** 打（`lua_getglobal(L, "print")` + 轮转参数）——server 模式下
 *      宿主会覆写 `print`（`output.cpp`），于是结果自动进 `out` 帧；本层不认识任何传输。
 *
 * 切片 D2b（`--repl --server`）：求值那一半（`repl_chunk_*` / `repl_eval_chunk`）从本地循环里
 *   抽出来，给服务端复用**同一份** `=` 简写 / 续行判定 / 错误码口径；服务端不再复制第二套。
 *   `repl_server_begin/chunk/end` 把 `lua_State` 与引擎会话的生命周期从「一次执行」拉长到
 *   「一条连接」（每次 `script` 帧 = 一行 / 一个完整块），非 `--repl` 的 server 路径一个字不改。
 *
 * 两条路径的收尾都过 `repl_unclosed_session_report`（D54 的安全网）：`lua_close` **之前**把
 *   registry 里没关的会话 detach（平台没有 `DebugActiveProcessStop` 时退 kill），报告只进
 *   stderr，**不改退出码**（仍是「最后一条命令的码」）。
 *
 * 退出码（`AGENTS.md` §3 的口径不变）：EOF → 返回**最后一条命令**的码；干净的 EOF 是 0。
 * ❗**本地取消不提供**（§11.6 第 4 条，已定）：`dbg` 的 `on_wait_timeout` 有意不读控制台，
 *   所以本地 REPL 里长 `pump` 无法 Ctrl+C —— 这是决定，不是 bug。计数钩子照装（纯 Lua
 *   死循环在 server 口径里能被 cancel 打断），本地模式下它是纯开销可忽略的空转。
 * ========================================================================== */
#include "repl.hpp"

#include "dbg_bindings.hpp" /* D54 安全网：dbg_open_session_count / dbg_release_unclosed_sessions */
#include "exec_guard.hpp"   /* guard_poll_control / guard_install_lua_hook（S13 / D32） */
#include "output.hpp"       /* install_print_override（server 的 `print` 进 `out` 帧） */
#include "state.hpp"        /* create_state_with_libs / encode_returns（同一个建状态入口） */

#include "core/core.hpp" /* DebuggerSession / set_current_session */
#include "core/util.hpp" /* PrintS / ErrorS / ErrorN / ErrorF（输出分流：控制台 vs 管道） */

#include <windows.h>

namespace autodbg::host
{
namespace
{
/* stdin 的一次读最多等这么久；片与片之间查一次控制帧（S13 要求 ≤250 ms）。 */
constexpr DWORD k_read_slice_ms = 100;

/* 一片的字节数 / 宽字符数。控制台路径先读宽字符再转 UTF-8，最坏 1 宽字符 → 3 字节。 */
constexpr unsigned k_read_cap = 1024;

/* 一行的上限（chunk 的上限是 `k_repl_chunk_cap`，在头文件里）。 */
constexpr unsigned k_line_cap = 4096;

const char k_prompt[]      = "autodbg> ";
const char k_prompt_cont[] = "   ...> ";

struct repl_input
{
    HANDLE   h;                  /* STD_INPUT_HANDLE；NULL / INVALID_HANDLE_VALUE ⇒ 立即 EOF */
    bool     console;            /* stdin 是真控制台（`GetConsoleMode` 成功）⇒ ReadConsoleW 兜底 */
    char     raw[k_line_cap];    /* 已经读到、还没切成行的字节 */
    unsigned raw_len;
    bool     eof;
};

void init_input(repl_input *in)
{
    in->h       = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode  = 0;
    in->console = (in->h != NULL && in->h != INVALID_HANDLE_VALUE &&
                   GetConsoleMode(in->h, &mode) != 0);
    in->raw_len = 0;
    in->eof     = (in->h == NULL || in->h == INVALID_HANDLE_VALUE);
}

/* 等 stdin 可读：切片 ≤100 ms，每片查一次控制帧（无执行连接时近乎零开销）。
 * 句柄不可等待（老系统的控制台句柄 / 特殊句柄）⇒ 直接放行，让下面那次读自己阻塞。 */
void wait_readable(repl_input *in)
{
    for (;;)
    {
        DWORD w = WaitForSingleObject(in->h, k_read_slice_ms);
        if (w == WAIT_TIMEOUT)
        {
            guard_poll_control();
            continue;
        }
        return;
    }
}

/* 读一片：>0 = 字节数，0 = EOF，-1 = 出错。
 * 顺序照 §4 第 9 条：先 `ReadFile`（管道/文件/重定向），失败且在控制台上再回退宽字符版。 */
int read_once(repl_input *in, char *dst, unsigned cap)
{
    DWORD got = 0;
    if (ReadFile(in->h, dst, cap, &got, NULL))
        return (int)got;

    /* ❗EOF 有两种表象：文件/重定向读到结尾是 `TRUE` + 0 字节，而**管道的写端关闭**是
     * `FALSE` + `ERROR_BROKEN_PIPE`（实测：`printf … | AutoDbg_86.exe --repl` 的第二次
     * ReadFile 就是这个）—— 不认这两个码就会把一个正常的 EOF 当成读错误。 */
    DWORD err = GetLastError();
    if (err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF)
        return 0;

    if (!in->console)
        return -1;

    wchar_t wbuf[k_read_cap];
    /* 宽字符数按 **cap/3** 要（否则 1024 个宽字符转 UTF-8 会超出 `dst`，截断可能
     * 切在字符中间 → 给 Lua 一段非法 UTF-8）。剩下的留在控制台缓冲里，下一次再读。 */
    unsigned wcap = cap / 3;
    if (wcap > k_read_cap)
        wcap = k_read_cap;
    if (wcap == 0)
        return -1;

    DWORD wn = 0;
    if (!ReadConsoleW(in->h, wbuf, wcap, &wn, NULL))
        return -1;
    if (wn == 0)
        return 0;

    /* 控制台读到的宽字符一律按 UTF-8 交给 Lua（两种变体的 Lua 都按字节吃源码；
     * 消息/提示符那边的宽窄转换由 `Print*` 各自处理）。 */
    int n = WideCharToMultiByte(CP_UTF8, 0, wbuf, (int)wn, dst, (int)cap, NULL, NULL);
    return (n > 0) ? n : -1;
}

/* 取一行（去掉行尾 CR/LF）：1 = 取到，0 = EOF（没有更多内容），-1 = 读失败。
 * `raw` 里多读的字节留给下一次调用（管道一次可能送来好几行）。 */
int read_line(repl_input *in, char *line, unsigned cap)
{
    unsigned scan = 0; /* raw 里已经确认「不是换行」的前缀长度（避免重复扫描） */

    for (;;)
    {
        for (unsigned i = scan; i < in->raw_len; i++)
        {
            if (in->raw[i] != '\n')
                continue;

            unsigned n = i;
            if (n > 0 && in->raw[n - 1] == '\r')
                n--;
            if (n >= cap)
                n = cap - 1;
            for (unsigned k = 0; k < n; k++)
                line[k] = in->raw[k];
            line[n] = '\0';

            unsigned rest = in->raw_len - (i + 1);
            for (unsigned k = 0; k < rest; k++)
                in->raw[k] = in->raw[i + 1 + k];
            in->raw_len = rest;
            return 1;
        }
        scan = in->raw_len;

        if (in->eof)
        {
            if (in->raw_len == 0)
                return 0;
            /* 最后一行没有换行符：照发（Lua 的 chunk 不要求结尾换行） */
            unsigned n = in->raw_len;
            if (n >= cap)
                n = cap - 1;
            for (unsigned k = 0; k < n; k++)
                line[k] = in->raw[k];
            line[n]     = '\0';
            in->raw_len = 0;
            return 1;
        }

        if (in->raw_len >= sizeof in->raw)
        {
            /* 超长行（4 KB 还没换行）：把手里这段当一行交出去，Lua 会报语法错。 */
            unsigned n = sizeof in->raw - 1;
            if (n >= cap)
                n = cap - 1;
            for (unsigned k = 0; k < n; k++)
                line[k] = in->raw[k];
            line[n]     = '\0';
            in->raw_len = 0;
            return 1;
        }

        char tmp[k_read_cap];
        wait_readable(in);
        int got = read_once(in, tmp, (unsigned)(sizeof tmp));
        if (got < 0)
            return -1;
        if (got == 0)
        {
            in->eof = true;
            continue;
        }
        for (int k = 0; k < got; k++)
        {
            if (in->raw_len < sizeof in->raw)
                in->raw[in->raw_len++] = tmp[k];
        }
    }
}

/* 打一条 Lua 错误（`where` = "syntax" / "runtime"，只进 stderr）。
 * 两种变体的 `lua_tolstring` 都给 UTF-8 字节；`ErrorN` 在 UNICODE 构建里先转宽字符。
 * ❗server 模式下 stderr 已经被 `output_begin` 装成 sink ⇒ 这条消息进 `out` 帧的 stderr 桶
 *   （`flags` bit0 = 1），客户端因此能看见 REPL 的报错。 */
void report_error(lua_State *L, const char *where)
{
    size_t      n   = 0;
    const char *err = lua_tolstring(L, -1, &n);

    autodbg::core::ErrorS("repl: ");
    autodbg::core::ErrorS(where);
    autodbg::core::ErrorS(" error: ");
    if (err != nullptr && n > 0)
        autodbg::core::ErrorN(err, n);
    else
        autodbg::core::ErrorS("<no message>");
    autodbg::core::ErrorS("\n");
}

/* 「在 EOF 处」的语法错 = 还没输完 ⇒ 继续读下一行（§11.6 第 2 条 / §4 第 4 条的多行续行）。
 * 实测两家文案：Lua 5.5 `unexpected symbol near <eof>`、LuaJIT `… near '<eof>'`
 * —— 都不是 plan 里写的 "end of input"，所以判据取**共同出现的 `<eof>` 标记**
 * （`near <eof>` / `near '<eof>'` 都命中；实测见 e2e_repl.ps1 的多行用例）。 */
bool error_is_incomplete(lua_State *L)
{
    size_t      n   = 0;
    const char *err = lua_tolstring(L, -1, &n);
    if (err == nullptr)
        return false;

    const char marker[] = "<eof>";
    size_t     mlen     = sizeof marker - 1;
    if (n < mlen)
        return false;
    for (size_t i = 0; i + mlen <= n; i++)
    {
        size_t k = 0;
        while (k < mlen && err[i + k] == marker[k])
            k++;
        if (k == mlen)
            return true;
    }
    return false;
}

/* 结果用**全局 `print`** 打（0 个返回值 ⇒ 什么也不打）。
 * 走全局而不是本层自己 printf：server 模式下宿主覆写的 `print` 会把输出送进 `out` 帧。 */
void print_results(lua_State *L, int base)
{
    int nres = lua_gettop(L) - base;
    if (nres <= 0)
        return;

    lua_getglobal(L, "print");
    if (!lua_isfunction(L, -1))
    {
        lua_pop(L, 1); /* 用户把 print 换成了非函数值：安静放弃（不是命令的错） */
        return;
    }
    lua_insert(L, base + 1); /* 函数挪到结果之前：print(v1, …, vn) */
    if (lua_pcall(L, nres, 0, 0) != 0)
    {
        report_error(L, "print");
        lua_pop(L, 1);
    }
}

/* 往 chunk 里追加，返回 false = 超过上限（调用方按语法错处理）。 */
bool chunk_append(char *chunk, unsigned *len, const char *s, unsigned n)
{
    if (*len + n >= k_repl_chunk_cap)
        return false;
    for (unsigned i = 0; i < n; i++)
        chunk[(*len)++] = s[i];
    return true;
}

unsigned cstr_len(const char *s)
{
    unsigned n = 0;
    while (s[n] != '\0')
        n++;
    return n;
}

/* D54 的安全网（本地与服务端共用）：把 registry 里**没关**的会话收尾，报告只进 stderr。
 * ❗必须在 `lua_close` **之前**调：会话的 callback ref / 事件表 ref 都挂在这个 state 上。
 * ❗不改退出码（退出码仍是「最后一条命令的码」）；单个会话失败只打一行、继续。 */
void repl_unclosed_session_report()
{
    int n = dbg_open_session_count();
    if (n <= 0)
        return;

    autodbg::core::ErrorF(TEXT("repl: %d unclosed session(s) - detaching\n"), n);
    (void)dbg_release_unclosed_sessions();
}
} /* namespace */

void repl_chunk_reset(repl_chunk *c)
{
    c->len = 0;
}

bool repl_chunk_feed(repl_chunk *c, const char *text, unsigned len)
{
    if (len > 0 && c->len == 0 && text[0] == '=')
    {
        /* `=expr` ⇒ `return expr`（§11.6 第 1 条；只在 chunk 为空时生效） */
        return chunk_append(c->buf, &c->len, "return ", 7) &&
               chunk_append(c->buf, &c->len, text + 1, len - 1);
    }
    return chunk_append(c->buf, &c->len, text, len) &&
           chunk_append(c->buf, &c->len, "\n", 1);
}

int repl_eval_chunk(lua_State *L, const char *chunk, size_t len, bool allow_continue, int *last_code,
                    const char **value_json)
{
    int status = luaL_loadbuffer(L, chunk, len, "=repl");

    if (status == 0)
    {
        int base = lua_gettop(L) - 1;
        int r    = lua_pcall(L, 0, LUA_MULTRET, 0);
        if (r != 0)
        {
            report_error(L, "runtime");
            *last_code = r; /* LUA_ERRRUN = 2（透传 Lua 的码，§3） */
        }
        else
        {
            /* T7 C-remote（D56）：返回值先取成 JSON（服务器 / 代理要它），再照旧 print 给
             * 本地 REPL 看。❗必须在 `lua_settop(L, base)` 之前（栈上才有返回值）。 */
            if (value_json != nullptr)
                *value_json = encode_returns(L, base);
            print_results(L, base);
            *last_code = 0;
        }
        lua_settop(L, base);
        return 0;
    }

    if (allow_continue && error_is_incomplete(L))
    {
        lua_pop(L, 1);
        return 1;
    }

    report_error(L, "syntax");
    *last_code = status; /* LUA_ERRSYNTAX = 3 */
    lua_pop(L, 1);
    return 0;
}

int run_repl(lua_State *L)
{
    if (L == nullptr)
        return -1;

    repl_input in;
    init_input(&in);

    DWORD out_mode    = 0;
    bool  show_prompt = (GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &out_mode) != 0);

    /* S13 的第②个注入点：纯 Lua 死循环靠计数钩子插话（本地模式下它只是空转）。 */
    guard_install_lua_hook(L);

    char       line[k_line_cap];
    repl_chunk chunk;
    int        last_code = 0;

    repl_chunk_reset(&chunk);

    for (;;)
    {
        if (show_prompt)
            autodbg::core::PrintS((chunk.len != 0) ? k_prompt_cont : k_prompt);

        int got = read_line(&in, line, (unsigned)(sizeof line));
        if (got < 0)
        {
            autodbg::core::ErrorS("repl: cannot read stdin\n");
            return -1;
        }

        bool eof_now = (got == 0);
        if (eof_now && chunk.len == 0)
            break; /* 干净的 EOF：退出码 = 最后一条命令的码（没跑过任何命令 = 0） */

        if (!eof_now)
        {
            if (!repl_chunk_feed(&chunk, line, cstr_len(line)))
            {
                autodbg::core::ErrorS("repl: line too long\n");
                last_code = LUA_ERRSYNTAX;
                repl_chunk_reset(&chunk);
                continue;
            }

            if (repl_eval_chunk(L, chunk.buf, chunk.len, true, &last_code) == 0)
                repl_chunk_reset(&chunk);
            continue;
        }

        /* EOF 但还有没输完的 chunk：让「在 EOF 处」的语法错真的报出来（退出码 3）。 */
        (void)repl_eval_chunk(L, chunk.buf, chunk.len, false, &last_code);
        repl_chunk_reset(&chunk);
        break;
    }

    /* D54 安全网：`lua_close`（调用方）之前把没关的会话收尾；不影响退出码。 */
    repl_unclosed_session_report();
    return last_code;
}

/* ------------------------------------------------------------------ `--repl --server`（D2b） */

repl_server *repl_server_begin()
{
    repl_server *r = new repl_server;
    r->L           = nullptr;
    r->sess        = new autodbg::core::DebuggerSession();

    /* 会话与脚本路径同口径（`exec_script` 也是这样挂的）：旧契约名走 `current_session()`。 */
    autodbg::core::set_current_session(r->sess);
    r->L = create_state_with_libs();
    if (r->L == nullptr)
    {
        autodbg::core::ErrorS("repl(server): cannot create lua state\n");
        delete r->sess;
        delete r;
        autodbg::core::set_current_session(nullptr);
        return nullptr;
    }

    /* 两件装具只在**连接**期装一次（不是每帧）：
     *   · `print` 覆写 —— 脚本的输出要进 `out` 帧（D30）；sink 本身仍按帧开关。
     *   · 计数钩子 —— 纯 Lua 死循环靠它被 `--cancel` 打断（D32）。 */
    install_print_override(r->L);
    guard_install_lua_hook(r->L);
    return r;
}

exec_result repl_server_chunk(repl_server *r, const char *src, size_t len)
{
    exec_result out;
    out.code       = -1;
    out.loaded     = false;
    out.oom        = false;
    out.elapsed_ms = 0;
    out.error[0]   = '\0';
    out.value_json = nullptr;

    if (r == nullptr || r->L == nullptr)
    {
        out.oom = true;
        return out;
    }

    /* 一次 `script` 帧 = **一行 / 一个完整块** ⇒ 不做跨帧的续行累积（`allow_continue = false`）：
     * 「没输完」在这里就是语法错（3），而不是等一条永远不会来的下一帧。
     * `=` 简写与错误码口径走本地 REPL 的同一份实现（`repl_chunk_feed` / `repl_eval_chunk`）。 */
    repl_chunk chunk;
    repl_chunk_reset(&chunk);
    if (!repl_chunk_feed(&chunk, src, (unsigned)len))
    {
        autodbg::core::ErrorS("repl: line too long\n");
        out.code = LUA_ERRSYNTAX;
        return out;
    }

    DWORD t0  = GetTickCount();
    int   last = 0;
    const char *value_json = nullptr;
    (void)repl_eval_chunk(r->L, chunk.buf, chunk.len, false, &last, &value_json);
    out.elapsed_ms = GetTickCount() - t0;

    out.code   = last;
    out.loaded = (last != LUA_ERRSYNTAX);
    /* T7 C-remote（D56）：这一帧的返回值 → `result` 帧的 `json.value`（口径同 `--json` 的 `result`）。
     * 没有返回值 / 失败路径 = nullptr（发帧那边写成 `null`，不另造一份字面量）。 */
    out.value_json = value_json;
    return out;
}

void repl_server_end(repl_server *r)
{
    if (r == nullptr)
        return;

    if (r->L != nullptr)
    {
        /* D54 安全网先跑（会话的 ref 挂在这个 state 上），然后才是 D1 的顺序：close → 释放会话。 */
        repl_unclosed_session_report();
        lua_close(r->L);
        r->L = nullptr;
    }

    delete r->sess;
    r->sess = nullptr;
    autodbg::core::set_current_session(nullptr);
    delete r;
}
} /* namespace autodbg::host */
