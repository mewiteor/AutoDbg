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
 * ========================================================================== */
#include "file_io.hpp"

#include "output.hpp"   /* T1（D50）：`images` 登记表 */
#include "stream.hpp"

#include "common/allocator.hpp"
#include "common/json_writer.hpp"

#include <stdio.h>
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
};

unsigned g_next_id = 0;

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

/* 规范化 mode：只认 w/wb/a/ab（其余按 wb） */
void normalize_mode(const char *src, char *dst, size_t cap)
{
    dst[0] = 'w';
    dst[1] = '\0';
    if (src == nullptr || src[0] == '\0')
        return;
    if (src[0] == 'a')
    {
        dst[0] = 'a';
        dst[1] = (strchr(src, 'b') != nullptr) ? 'b' : '\0';
        dst[2] = '\0';
        return;
    }
    dst[1] = (strchr(src, 'b') != nullptr) ? 'b' : '\0';
    dst[2] = '\0';
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
    normalize_mode(mode, norm, sizeof norm);
    bool text = (strchr(norm, 'b') == nullptr);

    file_obj *o = (file_obj *)lua_newuserdata(L, sizeof(file_obj));
    memset(o, 0, sizeof *o);
    o->text = text;
    memcpy(o->path, path, strlen(path) + 1);
    memcpy(o->mode, norm, strlen(norm) + 1);
    luaL_getmetatable(L, k_file_meta);
    lua_setmetatable(L, -2);

    if (!client_io_enabled())
    {
        /* 本地：就是真文件（与 io.open 等价） */
        o->fp = fopen(path, norm);
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
    if (!stream_write_frame(stream_frame_open, o->id, 0, w.data(), w.size(), nullptr, 0))
        return push_err(L, "open_file: stream send failed (client disconnected?)"), 2;
    record_written_path(o);
    return 1;
}

void create_meta(lua_State *L)
{
    luaL_newmetatable(L, k_file_meta);
    lua_newtable(L);
    lua_pushcfunction(L, file_write);
    lua_setfield(L, -2, "write");
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
} /* namespace autodbg::host */
