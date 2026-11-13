/* =============================================================================
 * src/scripting/lua/file_stream.cpp —— `open_file()` 与文件对象（流式 client 文件 I/O）
 *
 * 来源：新写。解决「回调里不能 yield ⇒ write_file 只能攒在内存里」的问题：
 *   · `open_file(path, mode)` 返回一个文件对象，`f:write()` 在**脚本线程**上直接写：
 *     本地模式 → 真文件；server 流式模式 → 一帧 `data` 直接 `send()` 上 `/run` 的连接
 *     （不 yield、不落盘、不占脚本内存）。
 *   · `f:flush()` 本地 `fflush`；流式模式恒 true（每帧本就即时发出）。
 *   · `f:close()` 发 `close` 帧 / `fclose`。
 * 约束：
 *   · 流式模式要求请求头 `X-AutoDbg-Stream: 1`（非流式请求里 `open_file` 直接报错，
 *     小数据请继续用 `write_file`）；
 *   · 本层不做文本行尾以外的编码转换：二进制安全，字节原样；
 *   · `__gc` 只做「尽力关闭 + 警告」，不会替调用方补数据。
 * T1（D50）：`open_file` 的 w/a 句柄会进 `images` 登记表（`output.hpp`）——只记路径，
 *   内容不动；去重与上限都在登记层，这里只在**真的打开成功**后记一笔。
 * T8 切片 a2（D63）：本地模式下**相对路径的基准是 `common/base_dir`**（env `AUTODBG_BASE_DIR` 优先，
 *   否则主 exe 所在目录），不是 CWD —— handoff 之后 `GetModuleFileName(NULL)` 在 child 里是 tmpdir
 *   那份，基准会漂，所以主 exe 把原始目录用 env 传过来。❗`o->path` 与 `images` 仍存调用方给的
 *   那一份（D50 的登记口径不变），只有真去打开时才用解析后的路径；`io.open` 与 `gui.screenshot_to`
 *   仍按 CWD（它们不是这层的口径），所以门禁的 `handoff.script-relative-base` 用 `open_file` 跑。
 * ========================================================================== */
#include "file_io.hpp"

#include "output.hpp"   /* T1（D50）：`images` 登记表 */
#include "stream.hpp"

#include "common/allocator.hpp"
#include "common/base_dir.hpp"   /* T8（D63）：相对路径的基准目录（env > 主 exe 目录） */
#include "common/frame.hpp"   /* T4：read_req/read_resp 的 u32 编解码 */
#include "common/json_writer.hpp"

#include <stdio.h>
#include <stdlib.h>   /* strtod：f:read("n") */
#include <string.h>

namespace autodbg::host
{
namespace
{
constexpr const char *k_file_meta = "AutoDbgFile";
constexpr size_t      k_path_max  = 512;

/* ❗故意不含默认成员初始化：这个对象是 `lua_newuserdata` 出来的裸内存，靠 memset 归零 */
struct file_obj
{
    FILE    *fp;
    unsigned id;
    size_t   written;
    bool     remote;
    bool     closed;
    bool     text;
    char     path[k_path_max];
    char     mode[8];

    /* ---- T4（D45/D49）：读向 ---- */
    bool           readable;
    bool           eof;
    unsigned char *rbuf;      /* 服务端读缓冲（远程才用） */
    size_t         rbuf_cap;  /* 0 → k_read_buf_default */
    size_t         rbuf_len;  /* 缓冲里的有效字节数 */
    size_t         rbuf_pos;  /* 已消费到哪（逻辑位置 = client_pos - (len-pos)） */
    long long      client_pos;/* client 侧文件位置（每次回包更新） */
    /* T4（D45 规则 3）：上一轮超时/取消后，可能有迟到的 `read_resp` 还在路上。
     * 守规矩的客户端收到 `cancel_notify` 就不发；不守规矩的还会发 —— 服务端必须自己丢掉它，
     * 否则它会被**下一次 read_req** 当成数据（=
     * 「读超时之后的下一次读拿到上一轮的旧数据」）。 */
    unsigned       stale_replies;
};

constexpr size_t k_read_buf_default = 4096;   /* D49：默认 4096（与 plan_cli.md §2.4 一致） */
constexpr size_t k_read_buf_max     = 8192;   /* 上限 = stream 层重组缓冲能装下的量 */

unsigned g_read_timeout_ms = 5000;   /* T4：一次文件操作的等待上限（main.cpp 用 boot 覆盖） */

/* 前置声明：lua_open_file 在读向实现之前（它要用 open 应答的缓冲） */
bool ensure_rbuf(file_obj *o);

unsigned g_next_id = 0;

/* T8（D63）：相对路径的基准目录。绝对路径原样返回；base 取不到也原样返回（不算失败）；
 * 结果装不下返回 nullptr（不写半个路径）。 */
const char *resolve_relative(const char *path, char *out, unsigned cap)
{
    if (common::path_is_absolute(path))
        return path;

    const char *base = common::base_dir();
    if (base == nullptr || base[0] == '\0')
        return path;

    return common::join_base_dir(base, path, out, cap) ? out : nullptr;
}

/* T1（D50）：写入模式（w / a）才进 `images`；纯读句柄不算「本次运行写出的文件」 */
void record_written_path(const file_obj *o)
{
    if (o->mode[0] == 'w' || o->mode[0] == 'a')
        output_record_path(o->path);
}

file_obj *to_obj(lua_State *L, int idx)
{
    return (file_obj *)luaL_checkudata(L, idx, k_file_meta);
}

void push_err(lua_State *L, const char *msg)
{
    lua_pushnil(L);
    lua_pushstring(L, msg);
}

/* 文本模式的 CRLF 归一（与 file_io.cpp 的 translate_write 同口径；那边是静态的，这里独立一份） */
char *to_crlf(const char *data, size_t n, size_t *out_n)
{
    size_t extra = 0;
    for (size_t i = 0; i < n; i++)
        if (data[i] == '\n' && (i == 0 || data[i - 1] != '\r'))
            extra++;
    char *out = (char *)common::alloc(n + extra + 1);
    if (out == nullptr)
    {
        *out_n = 0;
        return nullptr;
    }
    size_t o = 0;
    for (size_t i = 0; i < n; i++)
    {
        if (data[i] == '\n' && (i == 0 || data[i - 1] != '\r'))
            out[o++] = '\r';
        out[o++] = data[i];
    }
    out[o] = '\0';
    *out_n = o;
    return out;
}

/* 规范化 mode：认 r/rb/w/wb/a/ab（其余非法） */
bool normalize_mode(const char *src, char *dst, size_t cap)
{
    dst[0] = 'w';
    dst[1] = '\0';
    if (src == nullptr || src[0] == '\0' || cap < 3)
        return true;
    char kind = src[0];
    if (kind != 'r' && kind != 'w' && kind != 'a')
        return false;
    if (strchr(src, '+') != nullptr)
        return false;   /* r+/w+/a+ 先不接（语义与缓冲模型冲突，留到需要时再议） */
    dst[0] = kind;
    dst[1] = (strchr(src, 'b') != nullptr) ? 'b' : '\0';
    dst[2] = '\0';
    return true;
}

/* 影子 stdio.h 里没有 snprintf（也没有 lua_writestringerror）→ 消息都是字面量，手写拷贝 */
void set_err(char *dst, size_t cap, const char *s)
{
    size_t i = 0;
    if (dst == nullptr || cap == 0)
        return;
    while (i + 1 < cap && s[i] != '\0')
    {
        dst[i] = s[i];
        i++;
    }
    dst[i] = '\0';
}

/* 把一次 write 的字节送出去：本地写文件 / 流式发 data 帧 */
bool emit(file_obj *o, const char *data, size_t n, char *err, size_t err_cap)
{
    if (n == 0)
        return true;

    if (!o->remote)
    {
        if (o->fp == nullptr)
        {
            set_err(err, err_cap, "file not open");
            return false;
        }
        size_t w = fwrite(data, 1, n, o->fp);
        if (w != n)
        {
            set_err(err, err_cap, "short write");
            return false;
        }
        return true;
    }

    if (!stream_active())
    {
        set_err(err, err_cap, "stream closed (client disconnected?)");
        return false;
    }
    if (!stream_write_frame(stream_frame_data, o->id, 0, nullptr, 0, data, n))
    {
        set_err(err, err_cap, "stream send failed (client disconnected or too slow)");
        return false;
    }
    return true;
}

/* ------------------------------- 方法 ------------------------------- */
int file_write(lua_State *L)
{
    file_obj *o = to_obj(L, 1);
    if (o->closed)
        return push_err(L, "write: file already closed"), 2;

    int    top     = lua_gettop(L);
    size_t total   = 0;
    char   err[128];
    err[0] = '\0';

    for (int i = 2; i <= top; i++)
    {
        size_t      n    = 0;
        const char *data = lua_tolstring(L, i, &n); /* 数字会被转成字符串（与 io.write 一致） */
        if (data == nullptr)
            return push_err(L, "write: argument must be a string or number"), 2;

        char  *tmp   = nullptr;
        size_t use_n = n;
        const char *use = data;
        if (o->text)
        {
            tmp = to_crlf(data, n, &use_n);
            if (tmp == nullptr)
                return push_err(L, "write: out of memory"), 2;
            use = tmp;
        }
        bool ok = emit(o, use, use_n, err, sizeof err);
        if (tmp != nullptr)
            common::release(tmp);
        if (!ok)
            return push_err(L, err), 2;
        o->written += use_n;
        total += use_n;
    }

    lua_pushinteger(L, (lua_Integer)total);
    return 1;
}

int file_flush(lua_State *L)
{
    file_obj *o = to_obj(L, 1);
    if (!o->closed && !o->remote && o->fp != nullptr)
        fflush(o->fp);
    /* 流式模式：每帧已经 send 出去了，flush 无额外动作 */
    lua_pushboolean(L, 1);
    return 1;
}

int file_close(lua_State *L)
{
    file_obj *o = to_obj(L, 1);
    if (o->closed)
    {
        lua_pushboolean(L, 1);
        return 1;
    }

    if (o->remote)
    {
        if (stream_active() && !stream_write_frame(stream_frame_close, o->id, 0, nullptr, 0, nullptr, 0))
            return push_err(L, "close: stream send failed"), 2;
    }
    else if (o->fp != nullptr)
    {
        if (fclose(o->fp) != 0)
        {
            o->fp = nullptr;
            o->closed = true;
            return push_err(L, "close: fclose failed"), 2;
        }
        o->fp = nullptr;
    }
    o->closed = true;
    lua_pushboolean(L, 1);
    return 1;
}

int file_gc(lua_State *L)
{
    file_obj *o = (file_obj *)luaL_checkudata(L, 1, k_file_meta);
    if (o->closed)
        return 0;

    if (!o->remote)
    {
        if (o->fp != nullptr)
        {
            fclose(o->fp);
            o->fp = nullptr;
        }
        o->closed = true;
        return 0;
    }

    /* 远程：尽力补一个 close 帧（客户端收到后会关本地文件）；流已经不在了就只警告。 */
    if (stream_active() && stream_write_frame(stream_frame_close, o->id, 0, nullptr, 0, nullptr, 0))
    {
        o->closed = true;
        return 0;
    }
    o->closed = true;
    fprintf(stderr, "autodbg: file handle for %s was not closed; client may keep it open\n", o->path);
    return 0;
}

int file_tostring(lua_State *L)
{
    file_obj *o = to_obj(L, 1);
    lua_pushfstring(L, "AutoDbgFile(%s, %s, %s)", o->path, o->mode, o->remote ? "remote" : "local");
    return 1;
}

int lua_open_file(lua_State *L)
{
    if (lua_type(L, 1) != LUA_TSTRING)
        return push_err(L, "open_file: path must be a string"), 2;

    const char *path = lua_tostring(L, 1);
    const char *mode = (lua_type(L, 2) == LUA_TSTRING) ? lua_tostring(L, 2) : "wb";
    if (strlen(path) >= k_path_max)
        return push_err(L, "open_file: path too long"), 2;

    char norm[8];
    if (!normalize_mode(mode, norm, sizeof norm))
        return push_err(L, "open_file: unsupported mode"), 2;
    bool text     = (strchr(norm, 'b') == nullptr);
    bool readable = (norm[0] == 'r');

    file_obj *o = (file_obj *)lua_newuserdata(L, sizeof(file_obj));
    memset(o, 0, sizeof *o);
    o->text     = text;
    o->readable = readable;
    o->rbuf_cap = k_read_buf_default;
    memcpy(o->path, path, strlen(path) + 1);
    memcpy(o->mode, norm, strlen(norm) + 1);
    luaL_getmetatable(L, k_file_meta);
    lua_setmetatable(L, -2);

    if (!client_io_enabled())
    {
        /* 本地：就是真文件（与 io.open 等价），但相对路径的基准是 base_dir（T8 / D63）。
         * base 取不到（env 没有、主 exe 路径也读不到）→ 原样用，不算失败。 */
        char resolved[k_path_max];
        const char *openp = resolve_relative(path, resolved, (unsigned)sizeof resolved);
        if (openp == nullptr)
            return push_err(L, "open_file: relative path does not fit after base dir"), 2;

        o->fp = fopen(openp, norm);
        if (o->fp == nullptr)
            return push_err(L, "open_file: cannot open file"), 2;
        record_written_path(o);
        return 1;
    }
    /* server：必须走流式请求，否则没法在回调里写 */
    if (!stream_active())
        return push_err(L,
                        "open_file: 需要流式请求（请求头 X-AutoDbg-Stream: 1）；"
                        "小数据请直接用 write_file"),
               2;

    o->remote  = true;
    o->id      = ++g_next_id;

    autodbg::common::json_writer w;
    w.begin_object();
    w.key("path");
    w.string(o->path);
    w.key("mode");
    w.string(o->mode);
    w.end_object();

    /* T4：模式走 `flags`、路径走 `data`（我们的 C 客户端不看 json——D28）；
     * json 留着给 nvim 等旧客户端（T5 一起删）。 */
    unsigned kind = (o->mode[0] == 'r') ? 0u : (o->mode[0] == 'a') ? 2u : 1u;
    if (!stream_write_frame(stream_frame_open, o->id, kind, w.data(), w.size(), o->path, strlen(o->path) + 1))
        return push_err(L, "open_file: stream send failed (client disconnected?)"), 2;

    /* T4：**等客户端的 open 应答**（文件在客户端那台机器上，开不了要马上告诉脚本）。
     * 写模式以前是 fire-and-forget；现在两边统一——错误能落到实处。
     * ❗但 chunked（现行 HTTP）是单向往回的：拿不到回包就不等（T5 删 HTTP 后这条分支消失）。 */
    if (stream_can_wait())
    {
        if (!ensure_rbuf(o))
            return push_err(L, "open_file: out of memory"), 2;
        size_t   json_len = 0, dlen = 0;
        unsigned type = 0, flags = 0, rid = 0;
        long     r = stream_wait_frame(stream_frame_read_resp, o->id, o->rbuf, o->rbuf_cap, &json_len, &dlen,
                                       &type, &flags, &rid, g_read_timeout_ms);
        if (r == -2)
        {
            stream_cancel_notify(o->id);
            return push_err(L, "open_file: timeout"), 2;
        }
        if (r == -4)
        {
            stream_cancel_notify(o->id);
            return push_err(L, "open_file: cancelled"), 2;
        }
        if (r <= 0)
            return push_err(L, "open_file: io error"), 2;
        if ((flags & (unsigned)stream_resp_error) != 0)
            return push_err(L, "open_file: cannot open file on client"), 2;
    }

    record_written_path(o);
    return 1;
}

/* =============================================================================
 * T4（D45/D49）：读向
 *
 * 语义铉死的四条（Lua 5.5 / LuaJIT 必须一致）：
 *   · 默认 format = "l"（含行尾的 `\n` 会被去掉；`*` 前缀两个版本都接受）；
 *   · `f:read(整数)` = 读 N 字节（字符串 format 才是「读一个数」）；
 *   · EOF → `nil`；错误 → `nil, err`；
 *   · 字节精确：帧里传原始字节，**不做 CRLF 归一**。
 * 缓冲（D49）：每句柄一个 4096 的服务端缓冲；`f:tell()` 是**逻辑位置**
 *   （= client 位置 − 未消费的缓冲）；`f:seek()` 是往返且清空缓冲。
 * 阻塞与取消（D45）：等回包时切片 ≤ 25 ms（`stream_wait_frame` 内部），片间查取消；
 *   超时 → `nil, "timeout"`，被取消 → `nil, "cancelled"`；被丢掉的那一轮用
 *   `cancel_notify` 告知客户端。
 * ========================================================================== */

size_t read_buf_cap(const file_obj *o)
{
    size_t cap = (o->rbuf_cap != 0) ? o->rbuf_cap : k_read_buf_default;
    return (cap > k_read_buf_max) ? k_read_buf_max : cap;
}

bool ensure_rbuf(file_obj *o)
{
    if (o->rbuf != nullptr)
        return true;
    size_t cap = read_buf_cap(o);
    o->rbuf    = (unsigned char *)common::alloc(cap);
    if (o->rbuf == nullptr)
        return false;
    o->rbuf_cap = cap;
    return true;
}

/* 远程：发一个 read_req 并等 read_resp。成功时字节/位置已经落在 o->rbuf 里。 */
bool remote_req(file_obj *o, unsigned op, unsigned whence, unsigned arg, size_t *data_len, char *err,
                size_t err_cap)
{
    if (!ensure_rbuf(o))
    {
        set_err(err, err_cap, "out of memory");
        return false;
    }

    unsigned char payload[8];
    common::frame_put_u32(payload, arg);
    common::frame_put_u32(payload + 4, 0);

    /* 先把「上一轮迟到的那一份」捞掉（非阻塞），再发新的 read_req —— 顺序不能反，
     * 否则新请求的回包与旧回包在同一个句柄上无法区分（D45 规则 3）。 */
    while (o->stale_replies > 0)
    {
        unsigned st = 0, sf = 0, sid = 0;
        size_t   sjl = 0, sdl = 0;
        long     sr = stream_wait_frame(stream_frame_read_resp, o->id, o->rbuf, o->rbuf_cap, &sjl, &sdl, &st, &sf,
                                    &sid, 1 /* 1 ms：只薠已经到的 */);
        if (sr <= 0)
            break;   /* -2 = 没滞留帧；其它 = 错，交给下面的正式请求报 */
        o->stale_replies--;
    }

    if (!stream_write_frame(stream_frame_read_req, o->id, op | (whence << 4), nullptr, 0, payload, sizeof payload))
    {
        set_err(err, err_cap, "send failed (client disconnected?)");
        return false;
    }

    size_t   json_len = 0, dlen = 0;
    unsigned type = 0, flags = 0, rid = 0;
    long     r = stream_wait_frame(stream_frame_read_resp, o->id, o->rbuf, o->rbuf_cap, &json_len, &dlen, &type,
                                   &flags, &rid, g_read_timeout_ms);
    if (r == -4)
    {
        stream_cancel_notify(o->id);   /* D45 规则 4：让 client 丢掉那一轮 */
        o->stale_replies++;            /* 但它真发了的话，我们也要自己丢掉 */
        set_err(err, err_cap, "cancelled");
        return false;
    }
    if (r == -2)
    {
        stream_cancel_notify(o->id);   /* 超时：同样让 client 把迟到的那一轮丢掉 */
        o->stale_replies++;
        set_err(err, err_cap, "timeout");
        return false;
    }
    if (r <= 0)
    {
        set_err(err, err_cap, "io error");
        return false;
    }
    if ((flags & (unsigned)stream_resp_error) != 0)
    {
        set_err(err, err_cap, "client file error");
        return false;
    }
    if ((flags & (unsigned)stream_resp_eof) != 0)
        o->eof = true;

    /* json 段本协议恒为 0（数字走 data 的二进制）；防御性地跳过它的长度 */
    if (json_len != 0)
    {
        size_t body = dlen;
        for (size_t i = 0; i < body; i++)
            o->rbuf[i] = o->rbuf[json_len + i];
    }
    *data_len = dlen;
    return true;
}

/* 把缓冲填满一次。返回 false = EOF / 出错（`err` 为空 = EOF） */
bool refill(file_obj *o, char *err, size_t err_cap)
{
    err[0] = '\0';
    if (o->eof)
        return false;

    if (!ensure_rbuf(o))
    {
        set_err(err, err_cap, "out of memory");
        return false;
    }

    if (o->remote)
    {
        size_t n = 0;
        if (!remote_req(o, (unsigned)stream_read_bytes, 0, (unsigned)o->rbuf_cap, &n, err, err_cap))
            return false;
        o->rbuf_len   = n;
        o->rbuf_pos   = 0;
        o->client_pos += (long long)n;
        if (n == 0)
        {
            o->eof = true;
            return false;
        }
        return true;
    }

    size_t got = fread(o->rbuf, 1, o->rbuf_cap, o->fp);
    o->rbuf_len   = got;
    o->rbuf_pos   = 0;
    o->client_pos += (long long)got;
    if (got == 0)
    {
        o->eof = true;
        return false;
    }
    return true;
}

/* 把一个字节流动作抽成「读更多」：返回 false = EOF/错误 */
bool ensure_buffered(file_obj *o, char *err, size_t err_cap)
{
    if (o->rbuf_pos < o->rbuf_len)
        return true;
    return refill(o, err, err_cap);
}

/* `f:read(整数)` */
int read_n(lua_State *L, file_obj *o, size_t want)
{
    char   err[64] = {0};
    char  *out      = nullptr;
    size_t total    = 0;
    size_t cap      = 64;

    out = (char *)common::alloc(cap);
    if (out == nullptr)
        return push_err(L, "read: out of memory"), 2;

    while (total < want)
    {
        if (!ensure_buffered(o, err, sizeof err))
        {
            if (err[0] != '\0')
            {
                common::release(out);
                return push_err(L, err), 2;
            }
            break;   /* EOF */
        }
        size_t avail = o->rbuf_len - o->rbuf_pos;
        size_t take  = want - total;
        if (take > avail)
            take = avail;
        if (total + take > cap)
        {
            size_t ncap = cap * 2 + take;
            char  *p    = (char *)common::resize(out, ncap);
            if (p == nullptr)
            {
                common::release(out);
                return push_err(L, "read: out of memory"), 2;
            }
            out = p;
            cap = ncap;
        }
        memcpy(out + total, o->rbuf + o->rbuf_pos, take);
        o->rbuf_pos += take;
        total += take;
    }

    if (total == 0)
    {
        common::release(out);
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, out, total);
    common::release(out);
    return 1;
}

/* `f:read("l")`（去掉行尾；`\r\n` 也去掉 `\r`）与 `f:read("a")` */
int read_line(lua_State *L, file_obj *o)
{
    char   err[64] = {0};
    char  *out     = nullptr;
    size_t total   = 0;
    size_t cap     = 64;

    out = (char *)common::alloc(cap);
    if (out == nullptr)
        return push_err(L, "read: out of memory"), 2;

    for (;;)
    {
        if (!ensure_buffered(o, err, sizeof err))
        {
            if (err[0] != '\0')
            {
                common::release(out);
                return push_err(L, err), 2;
            }
            break;   /* EOF */
        }

        size_t avail = o->rbuf_len - o->rbuf_pos;
        size_t i     = 0;
        while (i < avail && o->rbuf[o->rbuf_pos + i] != '\n')
            i++;
        bool found = (i < avail);
        size_t take = found ? (i + 1) : i;

        if (total + take + 1 > cap)
        {
            size_t ncap = cap * 2 + take;
            char  *p    = (char *)common::resize(out, ncap);
            if (p == nullptr)
            {
                common::release(out);
                return push_err(L, "read: out of memory"), 2;
            }
            out = p;
            cap = ncap;
        }
        if (take > 0)
        {
            memcpy(out + total, o->rbuf + o->rbuf_pos, take);
            o->rbuf_pos += take;
            total += take;
        }
        if (found)
            break;
    }

    if (total == 0)
    {
        common::release(out);
        lua_pushnil(L);   /* EOF（与 io.read 一致） */
        return 1;
    }
    /* 去掉行尾的 `\n` 与可选的 `\r` */
    if (total > 0 && out[total - 1] == '\n')
        total--;
    if (total > 0 && out[total - 1] == '\r')
        total--;
    lua_pushlstring(L, out, total);
    common::release(out);
    return 1;
}

int read_all(lua_State *L, file_obj *o)
{
    char   err[64] = {0};
    char  *out     = nullptr;
    size_t total   = 0;
    size_t cap     = 256;

    out = (char *)common::alloc(cap);
    if (out == nullptr)
        return push_err(L, "read: out of memory"), 2;

    for (;;)
    {
        if (!ensure_buffered(o, err, sizeof err))
        {
            if (err[0] != '\0')
            {
                common::release(out);
                return push_err(L, err), 2;
            }
            break;
        }
        size_t avail = o->rbuf_len - o->rbuf_pos;
        if (total + avail + 1 > cap)
        {
            size_t ncap = cap * 2 + avail;
            char  *p    = (char *)common::resize(out, ncap);
            if (p == nullptr)
            {
                common::release(out);
                return push_err(L, "read: out of memory"), 2;
            }
            out = p;
            cap = ncap;
        }
        memcpy(out + total, o->rbuf + o->rbuf_pos, avail);
        o->rbuf_pos += avail;
        total += avail;
    }

    lua_pushlstring(L, out, total);
    common::release(out);
    return 1;
}

/* `f:read("n")`：读一个十进制数（跳过前导空白） */
int read_number(lua_State *L, file_obj *o)
{
    char   err[64] = {0};
    char   buf[64];
    size_t n = 0;

    /* 跳过空白 */
    for (;;)
    {
        if (!ensure_buffered(o, err, sizeof err))
            break;
        unsigned char ch = o->rbuf[o->rbuf_pos];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')
        {
            o->rbuf_pos++;
            continue;
        }
        break;
    }

    while (n + 1 < sizeof buf)
    {
        if (!ensure_buffered(o, err, sizeof err))
            break;
        unsigned char ch = o->rbuf[o->rbuf_pos];
        bool          num = (ch >= '0' && ch <= '9') || ch == '+' || ch == '-' || ch == '.' || ch == 'e' || ch == 'E';
        if (!num)
            break;
        buf[n++] = (char)ch;
        o->rbuf_pos++;
    }
    buf[n] = '\0';

    if (n == 0)
    {
        if (err[0] != '\0')
            return push_err(L, err), 2;
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, (lua_Number)strtod(buf, nullptr));
    return 1;
}

int file_read(lua_State *L)
{
    file_obj *o = to_obj(L, 1);
    if (o->closed)
        return push_err(L, "read: file is closed"), 2;
    if (!o->readable)
        return push_err(L, "read: file is not opened for reading"), 2;

    /* 无参数 → 默认 "l"（Lua 5.5 / LuaJIT 一致） */
    if (lua_gettop(L) < 2)
        return read_line(L, o);

    if (lua_type(L, 2) == LUA_TNUMBER)
    {
        lua_Integer want = lua_tointeger(L, 2);
        if (want < 0)
            return push_err(L, "read: invalid format"), 2;
        return read_n(L, o, (size_t)want);
    }

    if (lua_type(L, 2) != LUA_TSTRING)
        return push_err(L, "read: invalid format"), 2;

    const char *fmt = lua_tostring(L, 2);
    if (fmt[0] == '*')
        fmt++;   /* LuaJIT 接受 `*l` / Lua 5.5 也接受 */

    if (fmt[0] == 'a' || fmt[0] == 'A')
        return read_all(L, o);
    if (fmt[0] == 'l' || fmt[0] == 'L')   /* `L` 保留行尾（这里都按 `l` 的「去行尾」口径实现） */
        return read_line(L, o);
    if (fmt[0] == 'n')
        return read_number(L, o);
    return push_err(L, "read: invalid format"), 2;
}

int file_read_line(lua_State *L)
{
    file_obj *o = to_obj(L, 1);
    if (o->closed)
        return push_err(L, "read_line: file is closed"), 2;
    if (!o->readable)
        return push_err(L, "read_line: file is not opened for reading"), 2;
    return read_line(L, o);
}

int file_lines_iter(lua_State *L)
{
    file_obj *o = to_obj(L, lua_upvalueindex(1));
    if (o->closed)
        return push_err(L, "lines: file is closed"), 2;
    return read_line(L, o);
}

int file_lines(lua_State *L)
{
    file_obj *o = to_obj(L, 1);
    if (!o->readable)
        return push_err(L, "lines: file is not opened for reading"), 2;
    lua_pushvalue(L, 1);
    lua_pushcclosure(L, file_lines_iter, 1);
    return 1;
}

int file_seek(lua_State *L)
{
    file_obj *o = to_obj(L, 1);
    if (o->closed)
        return push_err(L, "seek: file is closed"), 2;

    const char *whence = (lua_type(L, 2) == LUA_TSTRING) ? lua_tostring(L, 2) : "cur";
    long long   offset = (lua_type(L, 3) == LUA_TNUMBER) ? (long long)lua_tointeger(L, 3) : 0;
    unsigned    w      = (unsigned)stream_whence_cur;
    if (whence[0] == 's')
        w = (unsigned)stream_whence_set;
    else if (whence[0] == 'e')
        w = (unsigned)stream_whence_end;

    /* 清空缓冲：seek 之后的逻辑位置以回包为准（D49） */
    o->rbuf_len = 0;
    o->rbuf_pos = 0;
    o->eof      = false;

    if (o->remote)
    {
        char   err[64] = {0};
        size_t dl      = 0;
        if (!remote_req(o, (unsigned)stream_read_seek, w, (unsigned)(int)offset, &dl, err, sizeof err))
            return push_err(L, err), 2;
        if (dl != 8)
            return push_err(L, "seek: bad reply"), 2;
        o->client_pos = (long long)common::frame_get_u32(o->rbuf);
        lua_pushinteger(L, (lua_Integer)o->client_pos);
        return 1;
    }

    int std_whence = (w == (unsigned)stream_whence_set) ? 0 /* SEEK_SET */ : (w == (unsigned)stream_whence_end) ? 2 /* SEEK_END */ : 1 /* SEEK_CUR */;
    if (fseek(o->fp, (long)offset, std_whence) != 0)
        return push_err(L, "seek: failed"), 2;
    long pos      = ftell(o->fp);
    o->client_pos = (pos < 0) ? 0 : (long long)pos;
    lua_pushinteger(L, (lua_Integer)o->client_pos);
    return 1;
}

int file_tell(lua_State *L)
{
    file_obj *o = to_obj(L, 1);
    if (o->closed)
        return push_err(L, "tell: file is closed"), 2;
    /* 逻辑位置 = client 位置 − 未消费的缓冲（D49） */
    long long buffered = (long long)(o->rbuf_len - o->rbuf_pos);
    lua_pushinteger(L, (lua_Integer)(o->client_pos - buffered));
    return 1;
}

int file_setbuf(lua_State *L)
{
    file_obj *o  = to_obj(L, 1);
    size_t    n  = (lua_type(L, 2) == LUA_TNUMBER) ? (size_t)lua_tointeger(L, 2) : k_read_buf_default;
    if (n == 0)
        n = 1;   /* 0 = 不预读（每次一个往返）：用 1 字节粒度表达 */
    if (n > k_read_buf_max)
        n = k_read_buf_max;

    if (o->rbuf != nullptr)
    {
        common::release(o->rbuf);
        o->rbuf = nullptr;
    }
    o->rbuf_cap = n;
    o->rbuf_len = 0;
    o->rbuf_pos = 0;
    lua_pushboolean(L, 1);
    return 1;
}

/* `f:close()` 已有；这里只是给 Lua 侧一个统一的「读错误」描述 */
int file_ok(lua_State *L)
{
    file_obj *o = to_obj(L, 1);
    if (o->closed)
    {
        lua_pushnil(L);
        lua_pushstring(L, "file is closed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

void create_meta(lua_State *L)
{
    luaL_newmetatable(L, k_file_meta);
    lua_newtable(L);
    lua_pushcfunction(L, file_write);
    lua_setfield(L, -2, "write");
    lua_pushcfunction(L, file_read);
    lua_setfield(L, -2, "read");
    lua_pushcfunction(L, file_read_line);
    lua_setfield(L, -2, "read_line");
    lua_pushcfunction(L, file_lines);
    lua_setfield(L, -2, "lines");
    lua_pushcfunction(L, file_seek);
    lua_setfield(L, -2, "seek");
    lua_pushcfunction(L, file_tell);
    lua_setfield(L, -2, "tell");
    lua_pushcfunction(L, file_setbuf);
    lua_setfield(L, -2, "setbuf");
    lua_pushcfunction(L, file_ok);
    lua_setfield(L, -2, "ok");
    lua_pushcfunction(L, file_flush);
    lua_setfield(L, -2, "flush");
    lua_pushcfunction(L, file_close);
    lua_setfield(L, -2, "close");
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, file_gc);
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, file_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);
}
} /* namespace */

void register_stream_file_api(lua_State *L)
{
    create_meta(L);
    lua_pushcfunction(L, lua_open_file);
    lua_setglobal(L, "open_file");
}

void set_client_read_timeout(unsigned ms)
{
    g_read_timeout_ms = (ms != 0) ? ms : 5000;
}
} /* namespace autodbg::host */
