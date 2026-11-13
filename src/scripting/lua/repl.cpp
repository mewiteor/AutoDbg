/* =============================================================================
 * src/scripting/lua/repl.cpp —— 模块侧 REPL 主循环（T7 切片 D1，plan_t7.md §11.7 的 D 行）
 *
 * 来源：新写（§4.3–4.5 的口径 + §11.6 的四个「对人的缺口」）。仓库原先没有 REPL。
 * 与旧实现的差异：无。
 *
 * 形状（三段，读 → 求值 → 打结果）：
 *   ① `repl_input` 把 stdin 的字节流切成行：先 `ReadFile`（**管道可跑**，门禁靠它），
 *      失败且是真控制台时回退 `ReadConsoleW`；每次「等可读」都是 ≤100 ms 的切片，片间
 *      `guard_poll_control()`（S13：REPL 的输入不能无限阻塞）。
 *   ② `eval_chunk` 把累积的 chunk 交给 `luaL_loadbuffer(L, …, "=repl")` + `lua_pcall`：
 *      「在 EOF 处」的语法错 = 还没输完 ⇒ 继续读行（多行续行）；其它语法错 / 运行错 ⇒
 *      打到 stderr 并把退出码记成 Lua 的码（3 / 2）。
 *   ③ 结果用**全局 `print`** 打（`lua_getglobal(L, "print")` + 轮转参数）——server 模式下
 *      宿主会覆写 `print`（`output.cpp`），于是结果自动进 `out` 帧；本层不认识任何传输。
 *
 * 退出码（`AGENTS.md` §3 的口径不变）：EOF → 返回**最后一条命令**的码；干净的 EOF 是 0。
 * ❗`--repl` + `--server` 属切片 D2（launcher 现在就拒绝），所以本文件没有归因/帧逻辑。
 * ❗**本地取消不提供**（§11.6 第 4 条，已定）：`dbg` 的 `on_wait_timeout` 有意不读控制台，
 *   所以本地 REPL 里长 `pump` 无法 Ctrl+C —— 这是决定，不是 bug。计数钩子照装（纯 Lua
 *   死循环在 server 口径里能被 cancel 打断），本地模式下它是纯开销可忽略的空转。
 * ========================================================================== */
#include "repl.hpp"

#include "exec_guard.hpp" /* guard_poll_control / guard_install_lua_hook（S13 / D32） */

#include "core/util.hpp" /* PrintS / ErrorS / ErrorN（输出分流：控制台 vs 管道） */

#include <windows.h>

namespace autodbg::host
{
namespace
{
/* stdin 的一次读最多等这么久；片与片之间查一次控制帧（S13 要求 ≤250 ms）。 */
constexpr DWORD k_read_slice_ms = 100;

/* 一片的字节数 / 宽字符数。控制台路径先读宽字符再转 UTF-8，最坏 1 宽字符 → 3 字节。 */
constexpr unsigned k_read_cap = 1024;

/* 一行与一个待求值 chunk 的上限。REPL 里超长行没有意义；超了就截断（Lua 会报语法错）。 */
constexpr unsigned k_line_cap  = 4096;
constexpr unsigned k_chunk_cap = 4096;

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
 * 两种变体的 `lua_tolstring` 都给 UTF-8 字节；`ErrorN` 在 UNICODE 构建里先转宽字符。 */
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

/* 求值一次累积的 chunk。返回 1 = 还没输完（调用方继续读下一行）；0 = 已处理。
 * `allow_continue`：false = 调用方已经到 EOF，此时「没输完」要按**语法错**上报
 * （退出码 3），而不是等永远不会来的下一行。`last_code` 只在返回 0 时更新。 */
int eval_chunk(lua_State *L, const char *chunk, size_t len, bool allow_continue, int *last_code)
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

/* 往 chunk 里追加，返回 false = 超过上限（调用方按语法错处理）。 */
bool chunk_append(char *chunk, unsigned *len, const char *s, unsigned n)
{
    if (*len + n >= k_chunk_cap)
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

unsigned cstr_copy(char *dst, const char *src, unsigned cap)
{
    unsigned n = 0;
    while (src[n] != '\0' && n + 1 < cap)
    {
        dst[n] = src[n];
        n++;
    }
    dst[n] = '\0';
    return n;
}
} /* namespace */

int run_repl(lua_State *L)
{
    if (L == nullptr)
        return -1;

    repl_input in;
    init_input(&in);

    DWORD out_mode   = 0;
    bool  show_prompt = (GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &out_mode) != 0);

    /* S13 的第②个注入点：纯 Lua 死循环靠计数钩子插话（本地模式下它只是空转）。 */
    guard_install_lua_hook(L);

    char     line[k_line_cap];
    char     chunk[k_chunk_cap];
    unsigned chunk_len = 0;
    int      last_code = 0;

    for (;;)
    {
        if (show_prompt)
            autodbg::core::PrintS((chunk_len != 0) ? k_prompt_cont : k_prompt);

        int got = read_line(&in, line, (unsigned)(sizeof line));
        if (got < 0)
        {
            autodbg::core::ErrorS("repl: cannot read stdin\n");
            return -1;
        }

        bool eof_now = (got == 0);
        if (eof_now && chunk_len == 0)
            break; /* 干净的 EOF：退出码 = 最后一条命令的码（没跑过任何命令 = 0） */

        if (!eof_now)
        {
            unsigned n = cstr_len(line);
            if (chunk_len == 0 && n > 0 && line[0] == '=')
            {
                /* `=expr` ⇒ `return expr`（§11.6 第 1 条；只在 chunk 为空时生效） */
                if (!chunk_append(chunk, &chunk_len, "return ", 7) ||
                    !chunk_append(chunk, &chunk_len, line + 1, n - 1))
                {
                    autodbg::core::ErrorS("repl: line too long\n");
                    last_code  = LUA_ERRSYNTAX;
                    chunk_len  = 0;
                    continue;
                }
            }
            else if (!chunk_append(chunk, &chunk_len, line, n) ||
                     !chunk_append(chunk, &chunk_len, "\n", 1))
            {
                autodbg::core::ErrorS("repl: line too long\n");
                last_code = LUA_ERRSYNTAX;
                chunk_len = 0;
                continue;
            }

            if (eval_chunk(L, chunk, chunk_len, true, &last_code) == 0)
                chunk_len = 0;
            continue;
        }

        /* EOF 但还有没输完的 chunk：让「在 EOF 处」的语法错真的报出来（退出码 3）。 */
        (void)eval_chunk(L, chunk, chunk_len, false, &last_code);
        chunk_len = 0;
        break;
    }

    return last_code;
}
} /* namespace autodbg::host */
