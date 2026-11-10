/* =============================================================================
 * src/scripting/lua/file_io.cpp —— `read_file` / `write_file` 实现（S7）
 *
 * 来源：新写。
 * 与旧实现的差异：无（新文件）。
 * 三种运行形态下的行为：
 *   · 本机脚本模式（`AutoDbg.exe script.lua`）：直接 fopen/fread/fwrite；
 *   · server 模式：`lua_yield` 一个效果表，宿主（server）把它转成 JSON 交给客户端，
 *     客户端执行后回传 `POST /effects`，宿主把结果值压回协程再 `lua_resume`；
 *   · 客户端不可用（没有挂起的宿主）：返回 nil + 错误串，不抛异常。
 * ❗`lua_yield` 只能从「被 resume 起来的协程」里调；server 的执行器就是这样跑的
 *   （见 exec.cpp 的 `exec_begin`）。本机脚本模式走直接路径，不 yield。
 * ========================================================================== */
#include "file_io.hpp"

#include "common/allocator.hpp"
#include "common/base64.hpp"
#include "common/json_writer.hpp"

#include <stdio.h>
#include <string.h>

namespace autodbg::host
{
namespace
{
bool g_client_io = false;

void push_err(lua_State *L, const char *msg)
{
    lua_pushnil(L);
    lua_pushstring(L, msg);
}

const char *path_arg(lua_State *L, int idx)
{
    return (lua_type(L, idx) == LUA_TSTRING) ? lua_tostring(L, idx) : nullptr;
}

/* opts 表里的整数字段（缺省 0） */
long long opt_int(lua_State *L, int table_idx, const char *key)
{
    if (!lua_istable(L, table_idx))
        return 0;
    lua_getfield(L, table_idx, key);
    long long v = (lua_type(L, -1) == LUA_TNUMBER) ? (long long)lua_tointeger(L, -1) : 0;
    lua_pop(L, 1);
    return v;
}

bool opt_bool(lua_State *L, int table_idx, const char *key)
{
    if (!lua_istable(L, table_idx))
        return false;
    lua_getfield(L, table_idx, key);
    bool v = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return v;
}

/* 解析 `read_file`/`write_file` 的模式参数：
 *   · 第二个/第三个参数是**字符串** → 就是模式（`io.open` 风格）；
 *   · 是**表** → `{ mode=, offset=, length=, append= }`（旧的写法继续可用）。 */
const char *parse_mode(lua_State *L, int idx, const char *def_mode)
{
    const char *mode = def_mode;
    if (idx < 1 || lua_gettop(L) < idx)
        return mode;
    int t = lua_type(L, idx);
    if (t == LUA_TSTRING)
        return lua_tostring(L, idx);
    if (t == LUA_TTABLE)
    {
        lua_getfield(L, idx, "mode");
        if (lua_type(L, -1) == LUA_TSTRING)
            mode = lua_tostring(L, -1); /* 字符串仍被表引用 → 指针有效 */
        lua_pop(L, 1);
    }
    return mode;
}

bool mode_is_text(const char *mode)
{
    return (mode == nullptr) ? false : (strchr(mode, 'b') == nullptr);
}

/* text 读：`\r\n` → `\n`（其余字节原样，含孤立的 `\r`） */
char *translate_read(const char *data, size_t n, size_t *out_n)
{
    *out_n = 0;
    if (data == nullptr || n == 0)
        return nullptr;
    char  *out = (char *)common::alloc(n);
    if (out == nullptr)
        return nullptr;
    size_t o = 0;
    for (size_t i = 0; i < n; i++)
    {
        if (data[i] == '\r' && i + 1 < n && data[i + 1] == '\n')
            continue; /* 丢掉 CR */
        out[o++] = data[i];
    }
    *out_n = o;
    return out;
}

/* text 写：先把 `\r\n` 归一成 `\n`，再把每个 `\n` 变成 `\r\n`（对 CRLF 输入幂等） */
char *translate_write(const char *data, size_t n, size_t *out_n)
{
    *out_n = 0;
    if (data == nullptr || n == 0)
        return nullptr;
    char  *out = (char *)common::alloc(n * 2 + 1); /* 最坏情况每个字节都变 CRLF */
    if (out == nullptr)
        return nullptr;
    size_t o = 0;
    for (size_t i = 0; i < n; i++)
    {
        if (data[i] == '\r' && i + 1 < n && data[i + 1] == '\n')
            continue; /* 归一化：跳过 CR，只保留 LF */
        if (data[i] == '\n')
        {
            out[o++] = '\r';
            out[o++] = '\n';
            continue;
        }
        out[o++] = data[i];
    }
    *out_n = o;
    return out;
}
/* yield 一个效果表给宿主（server 模式）。`nresults` 个值由宿主在 resume 时压回。 */
int yield_effect(lua_State *L, const char *op, const char *path, const char *mode, long long offset, long long length,
                 const char *data, size_t data_len, bool append)
{
    lua_createtable(L, 0, 8);
    lua_pushstring(L, op);
    lua_setfield(L, -2, "op");
    lua_pushstring(L, path);
    lua_setfield(L, -2, "path");
    lua_pushstring(L, mode);
    lua_setfield(L, -2, "mode");

    if (strcmp(op, "read_file") == 0)
    {
        lua_pushinteger(L, (lua_Integer)offset);
        lua_setfield(L, -2, "offset");
        lua_pushinteger(L, (lua_Integer)length);
        lua_setfield(L, -2, "length");
    }
    else
    {
        lua_pushlstring(L, (data != nullptr) ? data : "", data_len);
        lua_setfield(L, -2, "data");
        lua_pushboolean(L, append ? 1 : 0);
        lua_setfield(L, -2, "append");
    }

    /* yield 1 个值（效果表）；resume 时宿主压入的返回值就是本函数的返回值 */
    return lua_yield(L, 1);
}
} /* namespace */

void set_client_io(bool on)
{
    g_client_io = on;
}

bool client_io_enabled()
{
    return g_client_io;
}

bool local_read_file(const char *path, long long offset, long long length, bool text,
                     char **data, size_t *n, char *err, size_t err_cap)
{
    if (data != nullptr)
        *data = nullptr;
    if (n != nullptr)
        *n = 0;
    if (err != nullptr && err_cap > 0)
        err[0] = '\0';
    if (path == nullptr || path[0] == '\0')
    {
        if (err != nullptr && err_cap > 0)
            strcpy(err, "empty path");
        return false;
    }
    if (offset < 0)
        offset = 0;

    FILE *f = fopen(path, "rb");
    if (f == nullptr)
    {
        if (err != nullptr && err_cap > 0)
            strcpy(err, "cannot open file");
        return false;
    }

    fseek(f, 0, SEEK_END);
    long total = ftell(f);
    if (total < 0)
        total = 0;
    if (offset > total)
        offset = total;

    long long avail = (long long)total - offset;
    long long want  = (length > 0 && length < avail) ? length : avail;
    if (want > (long long)k_file_effect_max)
        want = (long long)k_file_effect_max;
    if (want < 0)
        want = 0;

    fseek(f, (long)offset, SEEK_SET);

    char *buf = (want > 0) ? (char *)common::alloc((size_t)want) : nullptr;
    if (want > 0 && buf == nullptr)
    {
        fclose(f);
        if (err != nullptr && err_cap > 0)
            strcpy(err, "out of memory");
        return false;
    }

    size_t got = (want > 0) ? fread(buf, 1, (size_t)want, f) : 0;
    fclose(f);

    if (text && got > 0)
    {
        size_t  tn  = 0;
        char   *tr  = translate_read(buf, got, &tn);
        if (buf != nullptr)
            common::release(buf);
        buf = tr;
        got = tn;
    }

    if (data != nullptr)
        *data = buf;
    else if (buf != nullptr)
        common::release(buf);
    if (n != nullptr)
        *n = got;
    return true;
}

bool local_write_file(const char *path, const void *data, size_t n, bool append, bool text,
                      size_t *written, char *err, size_t err_cap)
{
    if (written != nullptr)
        *written = 0;
    if (err != nullptr && err_cap > 0)
        err[0] = '\0';
    if (path == nullptr || path[0] == '\0')
    {
        if (err != nullptr && err_cap > 0)
            strcpy(err, "empty path");
        return false;
    }
    if (n > k_file_effect_max)
    {
        if (err != nullptr && err_cap > 0)
            strcpy(err, "data too large");
        return false;
    }

    const char *payload = (const char *)data;
    size_t      payload_n = n;
    char       *translated = nullptr;
    if (text && n > 0)
    {
        size_t tn = 0;
        translated = translate_write((const char *)data, n, &tn);
        if (translated == nullptr)
        {
            if (err != nullptr && err_cap > 0)
                strcpy(err, "out of memory");
            return false;
        }
        payload   = translated;
        payload_n = tn;
    }

    FILE *f = fopen(path, append ? "ab" : "wb");
    if (f == nullptr)
    {
        if (err != nullptr && err_cap > 0)
            strcpy(err, "cannot open file");
        return false;
    }
    size_t w = (payload_n > 0) ? fwrite(payload, 1, payload_n, f) : 0;
    fclose(f);
    if (translated != nullptr)
        common::release(translated);

    if (written != nullptr)
        *written = w;
    if (w != payload_n)
    {
        if (err != nullptr && err_cap > 0)
            strcpy(err, "short write");
        return false;
    }
    return true;
}

/* ------------------------------ Lua 绑定 ------------------------------ */
namespace
{
/* read_file(path [, {offset=, length=}]) → string | nil, err */
int lua_read_file(lua_State *L)
{
    const char *path = path_arg(L, 1);
    if (path == nullptr)
        return push_err(L, "read_file: path must be a string"), 2;

    long long   offset = opt_int(L, 2, "offset");
    long long   length = opt_int(L, 2, "length");
    const char *mode   = parse_mode(L, 2, "rb");
    bool        text   = mode_is_text(mode);

    if (g_client_io)
        return yield_effect(L, "read_file", path, mode, offset, length, nullptr, 0, false);

    char   *data = nullptr;
    size_t  n    = 0;
    char    err[128];
    if (!local_read_file(path, offset, length, text, &data, &n, err, sizeof err))
        return push_err(L, err), 2;

    lua_pushlstring(L, (data != nullptr) ? data : "", n);
    if (data != nullptr)
        common::release(data);
    return 1;
}

/* write_file(path, data [, {append=}]) → bytes written | nil, err */
int lua_write_file(lua_State *L)
{
    const char *path = path_arg(L, 1);
    if (path == nullptr)
        return push_err(L, "write_file: path must be a string"), 2;

    size_t      n    = 0;
    const char *data = (lua_type(L, 2) == LUA_TSTRING) ? lua_tolstring(L, 2, &n) : nullptr;
    if (data == nullptr && lua_type(L, 2) != LUA_TSTRING)
        return push_err(L, "write_file: data must be a string"), 2;

    const char *mode   = parse_mode(L, 3, "wb");
    bool        append = opt_bool(L, 3, "append") || (strchr(mode, 'a') != nullptr);
    bool        text   = mode_is_text(mode);

    if (g_client_io)
        return yield_effect(L, "write_file", path, mode, 0, 0, data, n, append);

    size_t written = 0;
    char   err[128];
    if (!local_write_file(path, data, n, append, text, &written, err, sizeof err))
        return push_err(L, err), 2;

    lua_pushinteger(L, (lua_Integer)written);
    return 1;
}
} /* namespace */

void register_file_api(lua_State *L)
{
    lua_pushcfunction(L, lua_read_file);
    lua_setglobal(L, "read_file");

    lua_pushcfunction(L, lua_write_file);
    lua_setglobal(L, "write_file");
}

/* ------------------------------ 效果协议助手 ------------------------------ */
bool effect_op(lua_State *L, int idx, char *buf, size_t cap)
{
    if (buf == nullptr || cap == 0)
        return false;
    buf[0] = '\0';
    if (!lua_istable(L, idx))
        return false;
    lua_getfield(L, idx, "op");
    bool ok = false;
    if (lua_type(L, -1) == LUA_TSTRING)
    {
        const char *s = lua_tostring(L, -1);
        size_t      n = strlen(s);
        if (n + 1 <= cap)
        {
            memcpy(buf, s, n + 1);
            ok = true;
        }
    }
    lua_pop(L, 1);
    return ok;
}

bool effect_to_json(lua_State *L, int idx, unsigned effect_id, char **out, size_t *out_len)
{
    if (out != nullptr)
        *out = nullptr;
    if (out_len != nullptr)
        *out_len = 0;
    if (!lua_istable(L, idx))
        return false;

    char op[24];
    if (!effect_op(L, idx, op, sizeof op))
        return false;

    lua_getfield(L, idx, "path");
    const char *path = (lua_type(L, -1) == LUA_TSTRING) ? lua_tostring(L, -1) : "";
    char path_copy[512];
    size_t pn = strlen(path);
    if (pn + 1 > sizeof path_copy)
        pn = sizeof path_copy - 1;
    memcpy(path_copy, path, pn);
    path_copy[pn] = '\0';
    lua_pop(L, 1);

    common::json_writer w;
    w.begin_object();
    w.key("id");
    w.number((unsigned long long)effect_id);
    w.key("op");
    w.string(op);
    w.key("path");
    w.string(path_copy, pn);

    lua_getfield(L, idx, "mode");
    const char *mode = (lua_type(L, -1) == LUA_TSTRING) ? lua_tostring(L, -1) : "";
    char mode_copy[8];
    size_t mn = strlen(mode);
    if (mn > sizeof mode_copy - 1)
        mn = sizeof mode_copy - 1;
    memcpy(mode_copy, mode, mn);
    mode_copy[mn] = '\0';
    lua_pop(L, 1);
    w.key("mode");
    w.string(mode_copy, mn);

    if (strcmp(op, "read_file") == 0)
    {
        lua_getfield(L, idx, "offset");
        long long offset = (lua_type(L, -1) == LUA_TNUMBER) ? (long long)lua_tointeger(L, -1) : 0;
        lua_pop(L, 1);
        lua_getfield(L, idx, "length");
        long long length = (lua_type(L, -1) == LUA_TNUMBER) ? (long long)lua_tointeger(L, -1) : 0;
        lua_pop(L, 1);
        w.key("offset");
        w.number(offset);
        w.key("length");
        w.number(length);
    }
    else
    {
        lua_getfield(L, idx, "data");
        size_t      dn    = 0;
        const char *data  = (lua_type(L, -1) == LUA_TSTRING) ? lua_tolstring(L, -1, &dn) : "";
        size_t      b64   = common::base64_encoded_size(dn) + 1;
        char       *enc   = (char *)common::alloc(b64);
        if (enc == nullptr)
        {
            lua_pop(L, 1);
            return false;
        }
        if (common::base64_encode(data, dn, enc, b64) == (size_t)-1)
            enc[0] = '\0';
        w.key("data");
        w.string(enc);
        common::release(enc);
        lua_pop(L, 1);

        lua_getfield(L, idx, "append");
        bool append = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        w.key("append");
        w.boolean(append);
    }

    w.end_object();
    if (w.overflow() || w.data() == nullptr)
        return false;

    char *buf = (char *)common::alloc(w.size() + 1);
    if (buf == nullptr)
        return false;
    memcpy(buf, w.data(), w.size());
    buf[w.size()] = '\0';

    if (out != nullptr)
        *out = buf;
    if (out_len != nullptr)
        *out_len = w.size();
    return true;
}
} /* namespace autodbg::host */
