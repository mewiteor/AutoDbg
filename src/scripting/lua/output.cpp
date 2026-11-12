/* =============================================================================
 * src/scripting/lua/output.cpp —— 输出收集器与 `print` 覆写实现
 *
 * 来源：新写（plan_server.md S1 / D30）。
 * 与旧实现的差异：无（新文件）。
 * 实现要点：
 *   ① sink 返回 `!echo`：echo 为真时把「写控制台」交回 core（它的控制台/重定向分流
 *      已经在 P7 里调好），收集与回显互不干扰；echo 为假时返回 true 表示已消费。
 *   ② `print` 覆写用「调全局 tostring」的办法（而不是 `luaL_tolstring`）：LuaJIT 的
 *      兼容层对 5.2 的 `luaL_tolstring` 支持不完全，用全局 `tostring` 两个版本一致。
 *   ③ 缓冲走 `common::alloc/resize/release`；`print` 的 echo 不做（见头文件取舍说明）。
 *   ④ T1（D50）：`images` 登记表 —— 固定 64 条 + 去重 + 丢弃计数，不越界；路径按
 *      UTF-8 存（就是 Lua 字符串原本的字节），`output_begin` 时清空。
 * ========================================================================== */
#include "output.hpp"

#include "core/util.hpp" /* set_output_sink / PrintN */

#include "stream.hpp"     /* 流式请求：输出直接当 `out` 帧发出去 */

#include "common/allocator.hpp"

#include <string.h>

namespace autodbg::host
{
namespace
{
constexpr size_t k_default_limit = 4u * 1024u * 1024u; /* 每条流 4 MB */
constexpr size_t k_max_paths     = 64;                 /* `images` 上限（T1 / D50） */

struct stream_buf
{
    char  *p   = nullptr;
    size_t len = 0;
    size_t cap = 0;
    bool   truncated = false;
};

struct collector
{
    bool       active = false;
    bool       echo   = true;
    size_t     limit  = k_default_limit;
    size_t     dropped = 0;
    stream_buf out;
    stream_buf err;
};

collector g_c;

/* `images` 登记表（T1 / D50）：本次运行写出的文件路径。不拥有 Lua 内存，
 * 所以存的是自己的副本（`common::alloc`），`output_begin` 时统一释放。 */
const char *g_paths[k_max_paths] = {};
size_t      g_path_count   = 0;
size_t      g_path_dropped = 0;

void paths_clear()
{
    for (size_t i = 0; i < g_path_count; i++)
    {
        if (g_paths[i] != nullptr)
            common::release((void *)g_paths[i]);
        g_paths[i] = nullptr;
    }
    g_path_count   = 0;
    g_path_dropped = 0;
}

void stream_free(stream_buf *b)
{
    if (b->p != nullptr)
        common::release(b->p);
    b->p         = nullptr;
    b->len       = 0;
    b->cap       = 0;
    b->truncated = false;
}

void stream_clear(stream_buf *b)
{
    b->len       = 0;
    b->truncated = false;
    if (b->p != nullptr)
        b->p[0] = '\0';
}

/* 追加（超上限就丢尾巴并记账；保证不切断 UTF-8 码点） */
void stream_append(stream_buf *b, collector *c, const char *data, size_t n)
{
    if (n == 0 || data == nullptr)
        return;
    if (b->truncated)
    {
        c->dropped += n;
        return;
    }

    size_t room = (c->limit > b->len) ? (c->limit - b->len) : 0;
    if (n > room)
    {
        /* 回退到 UTF-8 码点边界，避免响应里出现半个字符 */
        size_t keep = room;
        while (keep > 0 && ((unsigned char)data[keep] & 0xC0u) == 0x80u)
            keep--;
        n = keep;
        b->truncated = true;
        c->dropped += (room == 0) ? 1 : 0; /* 只做「有丢过」的语义，不必精确 */
    }
    if (n == 0)
        return;

    if (b->len + n + 1 > b->cap)
    {
        size_t cap = (b->cap == 0) ? 256 : b->cap;
        while (cap < b->len + n + 1)
            cap += cap / 2;
        char *p = (char *)common::resize(b->p, cap);
        if (p == nullptr)
        {
            b->truncated = true;
            return;
        }
        b->p   = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, data, n);
    b->len += n;
    b->p[b->len] = '\0';
}

/* core 的输出 sink：返回 true = 已消费（不回显） */
bool sink(void *ctx, int stream, const char *utf8, size_t len)
{
    (void)ctx;
    if (!g_c.active)
        return false;

    stream_buf *b = (stream == 2) ? &g_c.err : &g_c.out;
    stream_append(b, &g_c, utf8, len);

    /* echo 为真 → 返回 false，让 core 走它自己的控制台/重定向分流 */
    return !g_c.echo;
}

/* `print(...)` 的 C 实现：逐个参数 tostring，`\t` 分隔，结尾 `\n` */
int lua_adbg_print(lua_State *L)
{
    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++)
    {
        if (i > 1)
            output_write(1, "\t", 1);

        lua_getglobal(L, "tostring");
        lua_pushvalue(L, i);
        if (lua_pcall(L, 1, 1, 0) == 0)
        {
            size_t      len = 0;
            const char *s   = lua_tolstring(L, -1, &len);
            if (s != nullptr && len > 0)
                output_write(1, s, len);
            lua_pop(L, 1);
        }
        else
        {
            lua_pop(L, 1);
            const char *msg = "<tostring error>";
            output_write(1, msg, strlen(msg));
        }
    }
    output_write(1, "\n", 1);
    return 0;
}
} /* namespace */

bool output_begin(size_t limit_per_stream, bool echo_to_console)
{
    g_c.active  = true;
    g_c.echo    = echo_to_console;
    g_c.limit   = (limit_per_stream != 0) ? limit_per_stream : k_default_limit;
    g_c.dropped = 0;
    stream_clear(&g_c.out);
    stream_clear(&g_c.err);
    paths_clear();

    core::set_output_sink(sink, nullptr);
    return true;
}

void output_record_path(const char *path)
{
    if (!g_c.active || path == nullptr || path[0] == '\0')
        return;   /* 非收集期（本地直路）不记录：D50 只在 `--json` 时需要 */

    for (size_t i = 0; i < g_path_count; i++)
        if (strcmp(g_paths[i], path) == 0)
            return;   /* 去重：同一个文件只记首次（多次 f:write 不重复） */

    if (g_path_count >= k_max_paths)
    {
        g_path_dropped++;
        return;
    }

    size_t n    = strlen(path);
    char  *copy = (char *)common::alloc(n + 1);
    if (copy == nullptr)
    {
        g_path_dropped++;
        return;
    }
    memcpy(copy, path, n + 1);
    g_paths[g_path_count++] = copy;
}

size_t output_path_count()
{
    return g_path_count;
}

size_t output_path_dropped()
{
    return g_path_dropped;
}

const char *output_path_at(size_t idx)
{
    return (idx < g_path_count) ? g_paths[idx] : nullptr;
}

output_snapshot output_end()
{
    core::set_output_sink(nullptr, nullptr);
    g_c.active = false;

    output_snapshot s;
    s.out           = (g_c.out.p != nullptr) ? g_c.out.p : "";
    s.out_len       = g_c.out.len;
    s.out_truncated = g_c.out.truncated;
    s.err           = (g_c.err.p != nullptr) ? g_c.err.p : "";
    s.err_len       = g_c.err.len;
    s.err_truncated = g_c.err.truncated;
    s.dropped_bytes = g_c.dropped;
    return s;
}

bool output_active()
{
    return g_c.active;
}

void output_write(int stream, const char *data, size_t len)
{
    if (!g_c.active || data == nullptr || len == 0)
        return;

    /* 流式请求（`X-AutoDbg-Stream: 1`）：不攒缓冲，直接当 `out` 帧发出去（长会话也能实时看到）。
     * 帧由执行脚本的线程写 socket，与 `f:write` 走同一条路径 → 顺序天然一致。 */
    if (stream_active())
    {
        stream_write_frame(stream_frame_out, 0, (stream == 2) ? 1u : 0u, nullptr, 0, data, len);
        return;
    }

    stream_buf *b = (stream == 2) ? &g_c.err : &g_c.out;
    stream_append(b, &g_c, data, len);
}

void install_print_override(lua_State *L)
{
    lua_pushcfunction(L, lua_adbg_print);
    lua_setglobal(L, "print");
}
} /* namespace autodbg::host */
