/* =============================================================================
 * src/scripting/lua/client.cpp —— raw TCP 帧客户端（见 client.hpp 的设计说明）
 *
 * 来源：新写（T3 / plan_cli.md D42）。
 * 与 server 的关系：同一个帧协议（`stream.hpp` 的帧类型 + `common/frame.*` 的编解码），
 *   客户端只做「握手 → 发 script/control → 收帧」这一条路，不碰引擎。
 *
 * 为什么客户端不用 `stream_*`（服务端那套全局单连接状态）：客户端只需要一个连接，
 *   但要求**读**的语义是「等到有帧为止」而不是「执行期的非阻塞 poll」，直接用
 *   `net_recv` + `common::frame` 更直白；`stream.*` 的单例状态留给服务端。
 * ========================================================================== */
#include "client.hpp"

#include "stream.hpp" /* 帧类型 / 协议版本 / 控制命令枚举 */

#include "common/allocator.hpp"
#include "common/frame.hpp"
#include "common/json_writer.hpp"
#include "common/net.hpp"

#include "core/util.hpp" /* PrintN / PrintS / ErrorS / E */

#include <windows.h>

#include <stdio.h>   /* T4：本地文件后端（fopen/fread/fwrite） */
#include <string.h>

namespace autodbg::host
{
/* 一次连接的读缓冲（帧头 8 字节 + json + data）。
 * 定义在 `autodbg::host` 作用域（不是匿名命名空间）：`client.hpp` 只前向声明它，
 * T7 C-remote（D56）的 `remote_connect` / `remote_run_script` / `remote_close` 要把它
 * 交给 `dbg_bindings.cpp` 当句柄（T7 切片 C-remote 的「不写第二套 TCP」就靠这个）。 */
struct remote_conn
{
    long long      sock;
    unsigned       read_timeout_ms;
    unsigned       max_frame;
    unsigned char *buf;     /* 容量 = max_frame */
    unsigned       next_id; /* `script` 帧的命令号（D2a 的归因）：每发一帧 +1（u16 回绕） */
};

namespace
{
constexpr unsigned k_default_read_timeout_ms = 5000;
constexpr unsigned k_default_max_frame       = 8u * 1024u * 1024u;

/* result 帧的 flags 位（见 stream.hpp）：客户端据此组装 `--json` 信封，不必解 JSON */
enum
{
    k_result_flag_ok        = 0x01,
    k_result_flag_canceled  = 0x02,
    k_result_flag_timed_out = 0x04,
    k_result_flag_truncated = 0x08,
};

bool send_all(remote_conn *c, const void *p, size_t n)
{
    if (n == 0)
        return true;
    return autodbg::common::net_send_all(c->sock, p, n, (int)c->read_timeout_ms);
}

/* 写一帧：u32 长度前缀 + 8 字节头 + json + data（raw 分帧，D42） */
bool write_frame(remote_conn *c, unsigned type, unsigned id, unsigned flags, const char *json, size_t json_len,
                 const void *data, size_t data_len)
{
    autodbg::common::frame_header h;
    h.type     = type;
    h.flags    = flags;
    h.id       = id;
    h.data_len = (unsigned)data_len;

    unsigned char hdr[autodbg::common::k_frame_header_size];
    if (!autodbg::common::frame_header_encode(hdr, sizeof hdr, h))
        return false;

    size_t        payload = sizeof hdr + json_len + data_len;
    unsigned char pfx[4];
    if (!autodbg::common::frame_prefix_encode(pfx, sizeof pfx, payload))
        return false;

    return send_all(c, pfx, sizeof pfx) && send_all(c, hdr, sizeof hdr) && send_all(c, json, json_len) &&
           send_all(c, data, data_len);
}

/* 读满 n 字节：true = 成功；false = 对端关闭/出错/整体超时。
 * `deadline`（`GetTickCount()` 的毫秒时间戳，0 = 不设上限）只影响 `-2`（这一片没数据）的处置：
 *   · 0（`client_main` 用）：继续等 —— server 可能在跑长脚本，宁可等也不误判断线；
 *   · 非 0（远端代理用）：过了 deadline 就失败（代理在别人的脚本里被调，不能无上限阻塞）。 */
bool recv_exact(remote_conn *c, void *buf, size_t n, DWORD deadline)
{
    unsigned char *p   = (unsigned char *)buf;
    size_t         got = 0;
    while (got < n)
    {
        int r = autodbg::common::net_recv(c->sock, p + got, n - got, (int)c->read_timeout_ms);
        if (r > 0)
        {
            got += (size_t)r;
            continue;
        }
        if (r == -2)
        {
            if (deadline != 0 && GetTickCount() >= deadline)
                return false;
            continue;
        }
        return false;
    }
    return true;
}

/* 读一帧：成功返回 true，`*type/_id/_flags` 与 `buf`（json 在前、data 在后）有效。
 * `*json_len` / `*data_len` 给出切分（`deadline` 见 `recv_exact`）。 */
bool read_frame(remote_conn *c, unsigned *type, unsigned *id, unsigned *flags, size_t *json_len, size_t *data_len,
                DWORD deadline = 0)
{
    unsigned char pfx[4];
    if (!recv_exact(c, pfx, sizeof pfx, deadline))
        return false;
    size_t payload = 0;
    if (!autodbg::common::frame_prefix_decode(pfx, sizeof pfx, &payload) || payload > (size_t)c->max_frame)
        return false;

    unsigned char hdr[autodbg::common::k_frame_header_size];
    if (!recv_exact(c, hdr, sizeof hdr, deadline))
        return false;
    autodbg::common::frame_header fh;
    if (!autodbg::common::frame_header_decode(hdr, sizeof hdr, &fh))
        return false;

    size_t jl = 0;
    if (!autodbg::common::frame_json_len(payload, fh.data_len, &jl))
        return false;
    size_t body = jl + (size_t)fh.data_len;
    if (body > 0 && !recv_exact(c, c->buf, body, deadline))
        return false;

    *type     = fh.type;
    *id       = fh.id;
    *flags    = fh.flags;
    *json_len = jl;
    *data_len = (size_t)fh.data_len;
    return true;
}

/* ---------------------------------------------------------------- 本地文件后端
 * T4（D44/D45）：文件在**客户端**这台机器上。`open` 帧的路径走 `data`、模式走 `flags`
 * （D28：本进程不解 JSON；帧里那份 json {path,mode} 留给 nvim 等旧客户端，T5 一起删）。 */
constexpr int          k_max_files = 32;
struct local_file
{
    unsigned id;
    FILE    *fp;
    bool     used;
};
local_file g_files[k_max_files];

local_file *find_file(unsigned id)
{
    for (int i = 0; i < k_max_files; i++)
        if (g_files[i].used && g_files[i].id == id)
            return &g_files[i];
    return nullptr;
}

local_file *add_file(unsigned id, FILE *fp)
{
    for (int i = 0; i < k_max_files; i++)
    {
        if (!g_files[i].used)
        {
            g_files[i].used = true;
            g_files[i].id   = id;
            g_files[i].fp   = fp;
            return &g_files[i];
        }
    }
    return nullptr;
}

void drop_file(unsigned id)
{
    local_file *f = find_file(id);
    if (f == nullptr)
        return;
    if (f->fp != nullptr)
        fclose(f->fp);
    f->used = false;
    f->fp   = nullptr;
}

void close_all_files()
{
    for (int i = 0; i < k_max_files; i++)
        drop_file(g_files[i].id);
}

/* `read_resp` 的两种形态：纯标志 / 带 8 字节位置 */
bool reply_flags(remote_conn *c, unsigned id, unsigned flags)
{
    return write_frame(c, stream_frame_read_resp, id, flags, nullptr, 0, nullptr, 0);
}
bool reply_data(remote_conn *c, unsigned id, unsigned flags, const void *data, size_t n)
{
    return write_frame(c, stream_frame_read_resp, id, flags, nullptr, 0, data, n);
}
bool reply_pos(remote_conn *c, unsigned id, unsigned flags, long pos)
{
    unsigned char b[8];
    autodbg::common::frame_put_u32(b, (unsigned)pos);
    autodbg::common::frame_put_u32(b + 4, 0);
    return write_frame(c, stream_frame_read_resp, id, flags, nullptr, 0, b, sizeof b);
}

/* `open` 帧：flags = 0 读 / 1 写 / 2 追加；data = 路径（NUL 结尾） */
void client_open(remote_conn *c, unsigned id, unsigned flags, const char *path)
{
    const char *mode = (flags == 1u) ? "wb" : (flags == 2u) ? "ab" : "rb";
    FILE       *fp   = fopen(path, mode);
    if (fp == nullptr)
    {
        reply_flags(c, id, (unsigned)stream_resp_error);
        return;
    }
    if (add_file(id, fp) == nullptr)
    {
        fclose(fp);
        reply_flags(c, id, (unsigned)stream_resp_error);
        return;
    }
    reply_flags(c, id, 0);
}

/* `read_req`：flags = op | (whence << 4)；data = {u32 arg, u32 保留} */
void client_read_req(remote_conn *c, unsigned id, unsigned flags, const unsigned char *arg_data, size_t arg_len)
{
    local_file *f = find_file(id);
    if (f == nullptr || f->fp == nullptr)
    {
        reply_flags(c, id, (unsigned)stream_resp_error);
        return;
    }

    unsigned op     = flags & 0x0Fu;
    unsigned whence = (flags >> 4) & 0x0Fu;
    unsigned arg    = (arg_len >= 4) ? autodbg::common::frame_get_u32(arg_data) : 0;

    if (op == (unsigned)stream_read_seek)
    {
        int std_whence = (whence == (unsigned)stream_whence_set) ? SEEK_SET
                         : (whence == (unsigned)stream_whence_end) ? SEEK_END
                                                                   : SEEK_CUR;
        if (fseek(f->fp, (long)(int)arg, std_whence) != 0)
        {
            reply_flags(c, id, (unsigned)stream_resp_error);
            return;
        }
        reply_pos(c, id, 0, ftell(f->fp));
        return;
    }
    if (op == (unsigned)stream_read_tell)
    {
        reply_pos(c, id, 0, ftell(f->fp));
        return;
    }

    /* read / line：一次最多回 k_client_read_max 字节（服务端缓冲上限 8192） */
    constexpr size_t k_client_read_max = 8192;
    unsigned char    buf[k_client_read_max];
    size_t           want = (arg == 0) ? k_client_read_max : (size_t)arg;
    if (want > k_client_read_max)
        want = k_client_read_max;

    size_t got = 0;
    unsigned out_flags = 0;
    if (op == (unsigned)stream_read_line)
    {
        /* 读到一个换行（含）或满缓冲 */
        while (got < want)
        {
            int ch = getc(f->fp);
            if (ch == EOF)
            {
                out_flags |= (unsigned)stream_resp_eof;
                break;
            }
            buf[got++] = (unsigned char)ch;
            if (ch == '\n')
                break;
        }
    }
    else
    {
        got = fread(buf, 1, want, f->fp);
        if (got == 0)
            out_flags |= (unsigned)stream_resp_eof;
    }

    reply_data(c, id, out_flags, buf, got);
}
size_t token_bytes(const autodbg_boot *boot, char *out, size_t cap)
{
    out[0] = '\0';
    const char *t = boot->server_token;
    if (t == nullptr || t[0] == '\0' || strcmp(t, "none") == 0 || strcmp(t, "auto") == 0)
        return 0;
    size_t n = 0;
    while (t[n] != '\0' && n + 1 < cap)
    {
        out[n] = t[n];
        n++;
    }
    out[n] = '\0';
    return n;
}

/* 建 socket + 读缓冲（**不含**握手）：`client_main` 与远端代理共用这一段。
 * 成功 true；失败 false 并填 `*why`（`net_unavailable` = 本系统没有 winsock）与 `*oom`。 */
bool open_conn(const char *host, unsigned port, unsigned read_timeout_ms, unsigned max_frame, remote_conn *c,
               autodbg::common::net_status *why, bool *oom)
{
    *oom           = false;
    *why           = autodbg::common::net_startup();
    c->buf         = nullptr;
    c->next_id     = 0;
    c->sock        = -1;
    if (*why != autodbg::common::net_ok)
        return false;

    long long s = autodbg::common::net_connect(host, port, why);
    if (s < 0)
        return false;

    c->sock            = s;
    c->read_timeout_ms = (read_timeout_ms != 0) ? read_timeout_ms : k_default_read_timeout_ms;
    c->max_frame       = (max_frame != 0) ? max_frame : k_default_max_frame;
    c->buf             = (unsigned char *)autodbg::common::alloc(c->max_frame);
    if (c->buf == nullptr)
    {
        autodbg::common::net_close(s);
        c->sock = -1;
        *oom    = true;
        return false;
    }
    return true;
}

bool connect_to(const autodbg_boot *boot, remote_conn *c)
{
    using autodbg::core::ErrorS;
    using autodbg::core::PrintF;

    autodbg::common::net_status why = autodbg::common::net_ok;
    bool                        oom = false;
    if (open_conn(boot->remote_host, boot->remote_port, boot->read_timeout_ms, boot->max_frame, c, &why, &oom))
        return true;
    if (why == autodbg::common::net_unavailable)
    {
        ErrorS("client: 本系统没有可用的 winsock（ws2_32.dll / wsock32.dll 都载不进）\n");
        return false;
    }
    if (oom)
    {
        ErrorS("client: out of memory\n");
        return false;
    }

    ErrorS("client: 连不上 ");
    ErrorS(boot->remote_host);
    ErrorS(":");
    PrintF("%u", boot->remote_port);
    ErrorS("（");
    ErrorS(autodbg::common::net_status_name(why));
    ErrorS("）\n");
    return false;
}

/* 关连接并释放读缓冲（不释放 `c` 本身 —— 那归 `remote_close` / `client_main` 的栈对象） */
void close_conn(remote_conn *c)
{
    if (c->sock >= 0)
        autodbg::common::net_close(c->sock);
    c->sock = -1;
    if (c->buf != nullptr)
    {
        autodbg::common::release(c->buf);
        c->buf = nullptr;
    }
}

/* ---------------------------------------------------------------- 可复用的远端传输（T7 C-remote）
 * 错误串的拷贝：`err` 是调用方给的定长缓冲（ASCII；传不了中文 —— 它要端到 Lua 的 string）。 */
void set_err(char *err, size_t cap, const char *msg)
{
    if (err == nullptr || cap == 0)
        return;
    size_t n = 0;
    while (msg[n] != '\0' && n + 1 < cap)
    {
        err[n] = msg[n];
        n++;
    }
    err[n] = '\0';
}

/* 把 `c->buf` 的前 `n` 字节拷成 alloc 缓冲（给错误文案与 result 帧的 json 用） */
char *copy_buf(const remote_conn *c, size_t n)
{
    char *p = (char *)autodbg::common::alloc(n + 1);
    if (p == nullptr)
        return nullptr;
    for (size_t i = 0; i < n; i++)
        p[i] = (char)c->buf[i];
    p[n] = '\0';
    return p;
}


/* 读本地脚本文件（客户端侧；路径由启动器给，CP936 字节） */
char *read_script_file(const autodbg_boot *boot, size_t *out_len)
{
    using autodbg::core::ErrorS;

    *out_len = 0;
    const char *path = (boot->script_a != nullptr) ? boot->script_a : nullptr;
    if (path == nullptr)
    {
        ErrorS("client: --remote needs a script (unless a control command is given)\n");
        return nullptr;
    }

    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        ErrorS("client: cannot open script: ");
        ErrorS(path);
        ErrorS("\n");
        return nullptr;
    }

    /* ❗用 `GetFileSize` 而不是 `GetFileSizeEx`：后者在 Win95 上没有（api_check 的 w95 表卡出来的）。
     * 脚本上限 64 MB，DWORD 够用。 */
    DWORD size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size == 0 || size > (64u * 1024u * 1024u))
    {
        CloseHandle(h);
        ErrorS("client: bad script size\n");
        return nullptr;
    }

    char *p = (char *)autodbg::common::alloc((size_t)size + 1);
    if (p == nullptr)
    {
        CloseHandle(h);
        ErrorS("client: out of memory\n");
        return nullptr;
    }

    DWORD got = 0;
    BOOL  ok  = ReadFile(h, p, size, &got, NULL);
    CloseHandle(h);
    if (!ok || got != size)
    {
        autodbg::common::release(p);
        ErrorS("client: cannot read script\n");
        return nullptr;
    }
    p[got] = '\0';
    *out_len = (size_t)got;
    return p;
}

/* 控制命令：boot.remote_cmd（1..5）→ `stream_control_cmd`（0..4） */
unsigned map_cmd(unsigned remote_cmd)
{
    switch (remote_cmd)
    {
    case 1: return stream_control_cancel;
    case 2: return stream_control_kill;
    case 3: return stream_control_detach;
    case 4: return stream_control_health;
    case 5: return stream_control_version;
    default: return stream_control_health;
    }
}

/* 把服务端应答（ack / info / error）打到 stdout；返回退出码 */
int print_control_reply(const unsigned *type, const char *json, size_t json_len, bool with_stdout)
{
    if (with_stdout && json_len > 0)
    {
        autodbg::core::PrintN(json, json_len);
        autodbg::core::PrintS("\n");
    }
    if (type == nullptr)
        return 0;
    return 0;
}
} /* namespace */

remote_conn *remote_connect(const char *host, unsigned port, const char *token, unsigned read_timeout_ms,
                            unsigned max_frame, char *err, size_t errcap)
{
    if (err != nullptr && errcap > 0)
        err[0] = '\0';

    remote_conn *c = new remote_conn;
    if (c == nullptr)
    {
        set_err(err, errcap, "out of memory");
        return nullptr;
    }
    c->sock            = -1;
    c->buf             = nullptr;
    c->read_timeout_ms = 0;
    c->max_frame       = 0;
    c->next_id         = 0;

    autodbg::common::net_status why = autodbg::common::net_ok;
    bool                        oom = false;
    if (!open_conn(host, port, read_timeout_ms, max_frame, c, &why, &oom))
    {
        if (oom)
            set_err(err, errcap, "out of memory");
        else if (why == autodbg::common::net_unavailable)
            set_err(err, errcap, "no winsock (ws2_32.dll / wsock32.dll)");
        else
        {
            char msg[96];
            char *p = msg;
            const char *a = "connect failed: ";
            while (*a != '\0' && p + 1 < msg + sizeof msg) *p++ = *a++;
            const char *b = autodbg::common::net_status_name(why);
            while (*b != '\0' && p + 1 < msg + sizeof msg) *p++ = *b++;
            *p = '\0';
            set_err(err, errcap, msg);
        }
        delete c;
        return nullptr;
    }

    /* 握手：`id` = 协议版本、`data` = token（与 `client_main` 逐字节同一口径） */
    size_t tok_len = (token != nullptr) ? strlen(token) : 0;
    if (!write_frame(c, stream_frame_handshake, k_stream_proto_version, 0, nullptr, 0, token, tok_len))
    {
        set_err(err, errcap, "handshake failed");
        close_conn(c);
        delete c;
        return nullptr;
    }

    unsigned type = 0, id = 0, flags = 0;
    size_t   json_len = 0, data_len = 0;
    /* 握手有独立的 10 s 上限：连上了但对面不讲话时不把脚本卡死 */
    DWORD deadline = GetTickCount() + 10000;
    if (!read_frame(c, &type, &id, &flags, &json_len, &data_len, deadline))
    {
        set_err(err, errcap, "no handshake response");
        close_conn(c);
        delete c;
        return nullptr;
    }
    if (type == stream_frame_error)
    {
        /* 版本 / token 不符：把服务端的 json 原文带回去（调用方决定怎么显示） */
        char *msg = copy_buf(c, json_len);
        set_err(err, errcap, (msg != nullptr) ? msg : "rejected");
        autodbg::common::release(msg);
        close_conn(c);
        delete c;
        return nullptr;
    }
    if (type != stream_frame_info)
    {
        set_err(err, errcap, "unexpected handshake reply");
        close_conn(c);
        delete c;
        return nullptr;
    }
    return c;
}

const char *remote_run_script(remote_conn *c, const void *src, size_t len, const char *chunk, unsigned timeout_ms,
                              char *err, size_t errcap)
{
    if (err != nullptr && errcap > 0)
        err[0] = '\0';
    if (c == nullptr || c->sock < 0 || c->buf == nullptr)
    {
        set_err(err, errcap, "not connected");
        return nullptr;
    }

    const char *cn = (chunk != nullptr && chunk[0] != '\0') ? chunk : "remote";
    unsigned    id = c->next_id++ & 0xFFFFu;
    if (!write_frame(c, stream_frame_script, id, 0, cn, strlen(cn), src, len))
    {
        set_err(err, errcap, "send failed");
        return nullptr;
    }

    DWORD deadline = (timeout_ms != 0) ? (GetTickCount() + (DWORD)timeout_ms) : 0;
    char *result   = nullptr;

    for (;;)
    {
        unsigned type = 0, fid = 0, flags = 0;
        size_t   json_len = 0, data_len = 0;
        if (!read_frame(c, &type, &fid, &flags, &json_len, &data_len, deadline))
        {
            set_err(err, errcap, (deadline != 0 && GetTickCount() >= deadline) ? "timeout" : "connection closed");
            break;
        }
        if (type == stream_frame_result)
        {
            if (result != nullptr)
                autodbg::common::release(result);
            result = copy_buf(c, json_len);
            if (result == nullptr)
                set_err(err, errcap, "out of memory");
            continue;
        }
        if (type == stream_frame_end)
            break;
        if (type == stream_frame_error)
        {
            char *msg = copy_buf(c, json_len);
            set_err(err, errcap, (msg != nullptr) ? msg : "server error");
            autodbg::common::release(msg);
            break;
        }
        /* `out`(4) / `event`(16) / `open` / `data` / `read_req` / `close` / `info`：
         * 代理路径一律丢弃 —— 返回值的 payload 走 `result.value`，不走 `out`；
         * `open_file` 的帧在代理路径上**不支持**（见 `client.hpp` 的说明）。 */
    }

    if (result == nullptr)
    {
        if (err == nullptr || err[0] == '\0')
            set_err(err, errcap, "no result frame");
        return nullptr;
    }
    return result;
}

void remote_close(remote_conn *c)
{
    if (c == nullptr)
        return;
    close_conn(c);
    delete c;
}

int client_main(const autodbg_boot *boot)
{
    using autodbg::core::ErrorS;
    using autodbg::core::PrintN;
    using autodbg::core::PrintS;
    using namespace autodbg::common;

    remote_conn c;
    c.sock            = -1;
    c.buf             = nullptr;
    c.read_timeout_ms = k_default_read_timeout_ms;
    c.max_frame       = k_default_max_frame;

    if (!connect_to(boot, &c))
        return 2;

    /* ---- ① 握手：`id` = 协议版本，`data` = token ---- */
    char   tok[96];
    size_t tok_len = token_bytes(boot, tok, sizeof tok);
    if (!write_frame(&c, stream_frame_handshake, k_stream_proto_version, 0, nullptr, 0, tok, tok_len))
    {
        ErrorS("client: handshake failed\n");
        close_conn(&c);
        return 2;
    }

    unsigned type = 0, id = 0, flags = 0;
    size_t   json_len = 0, data_len = 0;
    if (!read_frame(&c, &type, &id, &flags, &json_len, &data_len))
    {
        ErrorS("client: no handshake response\n");
        close_conn(&c);
        return 2;
    }
    if (type == stream_frame_error)
    {
        /* 版本不符 / token 不符：把服务端的 json 原样打出去（调用方要看到原因） */
        PrintN((const char *)c.buf, json_len);
        PrintS("\n");
        close_conn(&c);
        return 2;
    }
    if (type != stream_frame_info)
    {
        ErrorS("client: unexpected handshake reply\n");
        close_conn(&c);
        return 2;
    }

    /* ---- ② 控制命令 ---- */
    if (boot->remote_cmd != 0)
    {
        unsigned cmd = map_cmd(boot->remote_cmd);
        if (!write_frame(&c, stream_frame_control, 0, cmd, nullptr, 0, nullptr, 0))
        {
            ErrorS("client: cannot send control frame\n");
            close_conn(&c);
            return 2;
        }

        int rc = 1;
        if (read_frame(&c, &type, &id, &flags, &json_len, &data_len))
        {
            if (json_len > 0)
            {
                PrintN((const char *)c.buf, json_len);
                PrintS("\n");
            }
            /* ack 的 `applied` 只能从 json 里看；D28 不允许客户端解析 JSON →
             * 用帧的 `flags` 传「applied」：服务端在 ack 帧里置 bit0。 */
            if (type == stream_frame_info)
                rc = 0;                                  /* health / version 恒成功 */
            else if (type == stream_frame_ack)
                rc = (flags & 0x01u) ? 0 : 1;            /* bit0 = applied */
            else
                rc = 2;
        }
        close_conn(&c);
        return rc;
    }

    /* ---- ③ 脚本 ---- */
    size_t src_len = 0;
    char  *src     = read_script_file(boot, &src_len);
    if (src == nullptr)
    {
        close_conn(&c);
        return -1;
    }

    bool json_mode = (boot->json_mode != 0 || boot->json_out != nullptr);

    /* `--json` 时先进内存（envelope 需要 stdout/stderr 两段）；否则直接写 */
    char  *out_buf = nullptr;
    char  *err_buf = nullptr;
    size_t out_len = 0, err_len = 0;
    size_t out_cap = 0, err_cap = 0;

    if (json_mode)
    {
        out_cap = 4096;
        err_cap = 1024;
        out_buf = (char *)alloc(out_cap);
        err_buf = (char *)alloc(err_cap);
        if (out_buf == nullptr || err_buf == nullptr)
        {
            ErrorS("client: out of memory\n");
            release(out_buf);
            release(err_buf);
            release(src);
            close_conn(&c);
            return -1;
        }
        out_buf[0] = '\0';
        err_buf[0] = '\0';
    }

    /* script 帧的 json 段 = chunkname（原文，不是 JSON）：服务端据此让 Lua 报
     * `你的文件:12: ...`，`--json` / 插件才能把错误行定位到文件。只用 basename。 */
    const char *chunk = (boot->script_a != nullptr) ? boot->script_a : "buffer.lua";
    for (const char *q = chunk; *q != '\0'; q++)
        if ((*q == '\\' || *q == '/') && q[1] != '\0')
            chunk = q + 1;
    if (!write_frame(&c, stream_frame_script, 0, 0, chunk, strlen(chunk), src, src_len))
    {
        ErrorS("client: cannot send script frame\n");
        release(out_buf);
        release(err_buf);
        release(src);
        close_conn(&c);
        return 2;
    }
    release(src);

    int    exit_code = 0;
    bool   r_ok = false, r_canceled = false, r_timed_out = false, r_truncated = false;
    bool   got_result = false;

    for (;;)
    {
        if (!read_frame(&c, &type, &id, &flags, &json_len, &data_len))
            break;

        if (type == stream_frame_out)
        {
            bool is_err = (flags & 0x01u) != 0;
            if (json_mode)
            {
                char **dst = is_err ? &err_buf : &out_buf;
                size_t *len = is_err ? &err_len : &out_len;
                size_t *cap = is_err ? &err_cap : &out_cap;
                if (*len + data_len + 1 > *cap)
                {
                    size_t want = *cap * 2 + data_len + 1;
                    char  *p    = (char *)resize(*dst, want);
                    if (p == nullptr)
                        continue;   /* 收不到就不进信封（不打断执行） */
                    *dst = p;
                    *cap = want;
                }
                memcpy(*dst + *len, c.buf + json_len, data_len);
                *len += data_len;
                (*dst)[*len] = '\0';
            }
            else if (data_len > 0)
            {
                /* 直通：stdout/stderr 各自的原句柄（重定向/`--quiet` 都能抓到） */
                autodbg::core::PrintExN(is_err ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE, (const char *)(c.buf + json_len),
                                        data_len);
            }
            continue;
        }
        if (type == stream_frame_open)
        {
            /* 路径在 data 段（NUL 结尾）、模式在 flags（D28：本进程不解 JSON） */
            char path[512];
            size_t n = data_len;
            if (n + 1 > sizeof path)
                n = sizeof path - 1;
            for (size_t i = 0; i < n; i++)
                path[i] = (char)c.buf[json_len + i];
            path[n] = '\0';
            client_open(&c, id, flags & 0x0Fu, path);
            continue;
        }
        if (type == stream_frame_data)
        {
            local_file *lf = find_file(id);
            if (lf != nullptr && lf->fp != nullptr && data_len > 0)
                fwrite(c.buf + json_len, 1, data_len, lf->fp);
            continue;
        }
        if (type == stream_frame_read_req)
        {
            client_read_req(&c, id, flags, (const unsigned char *)(c.buf + json_len), data_len);
            continue;
        }
        if (type == stream_frame_close)
        {
            drop_file(id);
            continue;
        }
        if (type == stream_frame_result)
        {
            got_result = true;
            exit_code  = (int)(short)id;   /* result 帧的 id = 退出码（u16 二补数） */
            r_ok        = (flags & k_result_flag_ok) != 0;
            r_canceled  = (flags & k_result_flag_canceled) != 0;
            r_timed_out = (flags & k_result_flag_timed_out) != 0;
            r_truncated = (flags & k_result_flag_truncated) != 0;

            /* 非 JSON 模式：失败时把服务端的结果 json 打到 stderr（那里带着 Lua 的
             * 错误文本），**不解析**——只是把诊断给出来（D28 不要求客户端看懂它）。 */
            if (!json_mode && exit_code != 0 && json_len > 0)
            {
                autodbg::core::PrintExN(STD_ERROR_HANDLE, (const char *)c.buf, json_len);
                PrintS("\n");
            }
            continue;
        }
        if (type == stream_frame_error)
        {
            if (json_len > 0)
            {
                autodbg::core::PrintExN(STD_ERROR_HANDLE, (const char *)c.buf, json_len);
                PrintS("\n");
            }
            exit_code = 2;
            break;
        }
        if (type == stream_frame_end)
            break;
        if (type == stream_frame_event)
            continue;   /* T7 D2a：命中记录（`event` 帧）—— 只用于归因，不进 `--json` 信封，也不打 stdout */
        /* info / 其它：忽略（握手时已经用过 info） */
    }

    close_conn(&c);

    if (json_mode)
    {
        json_writer w;
        w.begin_object();
        w.key("ok");
        w.boolean(got_result ? r_ok : (exit_code == 0));
        w.key("exit");
        w.number((long long)exit_code);
        w.key("canceled");
        w.boolean(r_canceled);
        w.key("timed_out");
        w.boolean(r_timed_out);
        w.key("truncated");
        w.boolean(r_truncated);
        w.key("pid");
        w.number((long long)boot->pid);   /* 远端 pid 不在这一帧里；显式给的才报 */
        w.key("stdout");
        w.string_escaped(out_buf != nullptr ? out_buf : "", out_len);
        w.key("stderr");
        w.string_escaped(err_buf != nullptr ? err_buf : "", err_len);
        w.key("images");
        w.begin_array();
        w.end_array();
        w.key("result");
        w.raw_escaped(nullptr, 0);   /* 远端脚本返回值不进信封（后续靠 result 帧扩展） */
        w.end_object();

        if (w.data() != nullptr)
        {
            if (boot->json_out != nullptr)
            {
                HANDLE h = CreateFileA(boot->json_out, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if (h != INVALID_HANDLE_VALUE)
                {
                    DWORD written = 0;
                    WriteFile(h, w.data(), (DWORD)w.size(), &written, NULL);
                    WriteFile(h, "\n", 1, &written, NULL);
                    CloseHandle(h);
                }
            }
            else
            {
                PrintN(w.data(), w.size());
                PrintS("\n");
            }
        }
        release(out_buf);
        release(err_buf);
    }

    return exit_code;
}
} /* namespace autodbg::host */
