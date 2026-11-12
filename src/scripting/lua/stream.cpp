/* =============================================================================
 * src/scripting/lua/stream.cpp —— 双向帧协议的传输层实现（见 stream.hpp 的分层说明）
 *
 * 来源：原「流式 `/run` 响应（帧 + chunked）」的写半边；T2 扩成双向协议：
 *   · 帧头打包/解包搬到 `common/frame.*`（可被 `--selftest` 断言）——本文件只剩 I/O；
 *   · 新增传输钩子（`stream_transport`）与两种分帧：chunked（现行 HTTP）/ raw（T3 的 TCP）；
 *   · 新增读方向 `stream_read_frame`：执行连接的**唯一 reader** 是脚本线程（D45），
 *     所以这里不做队列、不加锁，超时一律返回 -2 让调用方切片重试 + 查取消标志。
 *
 * 关键约束（为什么这么写）：
 *   · 帧由**执行脚本的那条线程**直接写 socket —— `f:write` 因此不需要 yield，回调里也能用；
 *   · 不缓冲整条流、不落盘：每帧发完就走，内存只占调用方给的那一块；
 *   · 背压交给 socket：`net_send_all` 带超时，客户端断了/太慢就返回失败（上层报错，不卡死）。
 * ========================================================================== */
#include "stream.hpp"

#include "common/frame.hpp"
#include "common/net.hpp" /* net_send_all / net_recv（已带 select 超时） */

#include <string.h>

namespace autodbg::host
{
namespace
{
long long         g_sock    = -1;
unsigned          g_timeout = 8000;
bool              g_active  = false;
int               g_framing = stream_framing_chunked;
stream_transport  g_tp      = {nullptr, nullptr, nullptr, nullptr};

/* 执行期 poll 的重组缓冲（D32）：只应付 `control` / `end` 这种小帧。
 * ❗脚本线程是唯一 writer/reader，不需要锁；`stream_begin_ex` / `stream_begin` 时清零。
 * 布局 = 4 字节长度前缀 + 8 字节帧头 + json（≤ k_ctrl_max）。 */
enum
{
    k_ctrl_max    = 4096,
    k_poll_buf_sz = 4 + 8 + k_ctrl_max,
};
unsigned char     g_poll_buf[k_poll_buf_sz];
size_t            g_poll_have = 0;
stream_control_fn g_ctrl_fn   = nullptr;
void             *g_ctrl_ctx  = nullptr;

/* 默认传输（winsock）：winsock 句柄用 `void*` 承载（`long long` → 指针，x86/x64 都够） */
bool sock_send(void *ctx, const void *data, size_t len)
{
    (void)ctx;
    if (g_sock < 0 || len == 0)
        return len == 0;   /* 空写视为成功（与 net_send_all 的语义对齐） */
    return autodbg::common::net_send_all(g_sock, data, len, (int)g_timeout);
}

int sock_recv(void *ctx, void *buf, size_t cap)
{
    (void)ctx;
    if (g_sock < 0)
        return -1;
    return autodbg::common::net_recv(g_sock, buf, cap, (int)g_timeout);
}

int sock_recv_nowait(void *ctx, void *buf, size_t cap)
{
    (void)ctx;
    if (g_sock < 0)
        return -1;
    return autodbg::common::net_recv(g_sock, buf, cap, 0);   /* 0 ms = 只查一下，不阻塞 */
}

bool tp_send(const void *data, size_t len)
{
    if (len == 0)
        return true;
    if (g_tp.send == nullptr)
        return false;
    return g_tp.send(g_tp.ctx, data, len);
}

/* 读满 n 字节（跨多次 recv）：>0 成功 / 0 对端在帧中间关闭 / -1 错误 / -2 超时 */
int tp_recv_exact(void *buf, size_t n)
{
    if (g_tp.recv == nullptr)
        return -1;
    unsigned char *p    = (unsigned char *)buf;
    size_t         got  = 0;
    int            idle = 0;
    while (got < n)
    {
        int r = g_tp.recv(g_tp.ctx, p + got, n - got);
        if (r > 0)
        {
            got += (size_t)r;
            idle = 0;
            continue;
        }
        if (r == 0)
            return (got == 0) ? 0 : -1;   /* 干净关闭 vs 帧中间断 */
        if (r == -2)
        {
            /* 超时：首次原样上报（调用方切片重试）；已经读了一半就继续等，
             * 因为协议要求「一帧要么完整要么连接作废」——攒够才返回。 */
            if (got == 0)
                return -2;
            idle++;
            if (idle > 200)   /* 连续 200 次超时（≈ 20 s）还没补齐 → 当断线 */
                return -1;
            continue;
        }
        return -1;
    }
    return (int)got;
}

/* chunk 头："<hex>\r\n"（小写十六进制；返回写进 buf 的字节数） */
size_t make_chunk_head(char *buf, size_t n)
{
    char   tmp[16];
    size_t k = 0;
    if (n == 0)
    {
        buf[0] = '0';
        buf[1] = '\r';
        buf[2] = '\n';
        return 3;
    }
    while (n > 0 && k < sizeof tmp)
    {
        unsigned d = (unsigned)(n & 0xFu);
        tmp[k++]   = (char)((d < 10) ? ('0' + d) : ('a' + (d - 10)));
        n >>= 4;
    }
    size_t w = 0;
    while (k > 0)
        buf[w++] = tmp[--k];
    buf[w++] = '\r';
    buf[w++] = '\n';
    return w;
}
} /* namespace */

bool stream_begin(long long sock, unsigned send_timeout_ms)
{
    if (sock < 0)
        return false;

    g_sock    = sock;
    g_timeout = (send_timeout_ms > 0) ? send_timeout_ms : 8000;
    g_framing = stream_framing_chunked;
    g_tp.send = sock_send;
    g_tp.recv = sock_recv;
    g_tp.recv_nowait = sock_recv_nowait;
    g_tp.ctx  = nullptr;
    g_active  = false;
    g_poll_have = 0;

    static const char k_head[] = "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: application/x-autodbg-stream\r\n"
                                 "Transfer-Encoding: chunked\r\n"
                                 "Connection: close\r\n"
                                 "X-AutoDbg-Stream: 1\r\n"
                                 "\r\n";
    if (!tp_send(k_head, sizeof k_head - 1))
    {
        g_sock = -1;
        return false;
    }
    g_active = true;
    return true;
}

bool stream_begin_ex(const stream_transport &tp, int framing, unsigned send_timeout_ms)
{
    if (tp.send == nullptr)
        return false;
    if (framing != stream_framing_chunked && framing != stream_framing_raw)
        return false;

    g_sock    = -1;                 /* 传输由 tp 负责，不碰 winsock */
    g_timeout = (send_timeout_ms > 0) ? send_timeout_ms : 8000;
    g_framing = framing;
    g_tp      = tp;
    g_poll_have = 0;                /* 新的连接：重组缓冲从头算 */
    g_active  = true;               /* 分帧层自己起头（raw 没有握手串要发） */
    return true;
}

void stream_end()
{
    g_active  = false;
    g_sock    = -1;
    g_tp.send = nullptr;
    g_tp.recv = nullptr;
    g_tp.recv_nowait = nullptr;
    g_tp.ctx  = nullptr;
    g_poll_have = 0;
    g_ctrl_fn   = nullptr;
    g_ctrl_ctx  = nullptr;
}

bool stream_active()
{
    return g_active;
}

bool stream_write_frame(unsigned type,
                        unsigned id,
                        unsigned flags,
                        const char *json,
                        size_t      json_len,
                        const void *data,
                        size_t      data_len)
{
    if (!g_active)
        return false;

    common::frame_header h;
    h.type     = type;
    h.flags    = flags;
    h.id       = id;
    h.data_len = (unsigned)data_len;

    unsigned char hdr[common::k_frame_header_size];
    if (!common::frame_header_encode(hdr, sizeof hdr, h))
        return false;

    size_t payload = (size_t)common::k_frame_header_size + json_len + data_len;
    if (payload > (size_t)common::k_frame_max_payload)
        return false;

    bool ok = true;
    if (g_framing == stream_framing_chunked)
    {
        char   chunk_head[24];
        size_t hn          = make_chunk_head(chunk_head, payload);
        static const char k_crlf[] = "\r\n";
        ok = tp_send(chunk_head, hn) && tp_send(hdr, sizeof hdr) && tp_send(json, json_len) &&
             tp_send(data, data_len) && tp_send(k_crlf, 2);
    }
    else
    {
        unsigned char pfx[4];
        ok = common::frame_prefix_encode(pfx, sizeof pfx, payload) && tp_send(pfx, sizeof pfx) &&
             tp_send(hdr, sizeof hdr) && tp_send(json, json_len) && tp_send(data, data_len);
    }

    if (!ok)
    {
        /* 客户端断开/超时：标记流失效，后续 `f:write` 立刻拿到错误而不是反复等超时 */
        g_active = false;
    }
    return ok;
}

long stream_read_frame(unsigned char *hdr,
                       void          *buf,
                       size_t         cap,
                       size_t        *out_json_len,
                       unsigned       max_frame)
{
    if (!g_active || g_tp.recv == nullptr || hdr == nullptr)
        return -1;
    if (g_framing != stream_framing_raw)
        return -3;   /* chunked 只有客户端读；服务端（本进程）只写不读 */

    unsigned limit = (max_frame != 0) ? max_frame : (unsigned)common::k_frame_max_payload;

    /* ① 长度前缀 */
    unsigned char pfx[4];
    int           r = tp_recv_exact(pfx, sizeof pfx);
    if (r <= 0)
        return (r == 0) ? 0 : r;   /* 0 = 对端关闭；-1 / -2 原样上报 */
    size_t payload = 0;
    if (!common::frame_prefix_decode(pfx, sizeof pfx, &payload) || payload > (size_t)limit)
        return -3;

    /* ② 8 字节帧头 */
    r = tp_recv_exact(hdr, (size_t)common::k_frame_header_size);
    if (r <= 0)
        return (r == 0) ? -1 : r;

    common::frame_header fh;
    if (!common::frame_header_decode(hdr, (size_t)common::k_frame_header_size, &fh))
        return -3;

    /* ③ json + data（json 长度由 payload 总长反推） */
    size_t json_len = 0;
    if (!common::frame_json_len(payload, fh.data_len, &json_len))
        return -3;
    size_t body = json_len + (size_t)fh.data_len;
    if (body > 0)
    {
        if (buf == nullptr || body > cap)
            return -3;   /* 调用方的缓冲放不下整帧：连接作废（不做部分消费） */
        r = tp_recv_exact(buf, body);
        if (r <= 0)
            return (r == 0) ? -1 : r;
    }
    if (out_json_len != nullptr)
        *out_json_len = json_len;
    return (long)payload;
}

bool stream_finish()
{
    if (!g_active)
        return false;
    if (g_framing != stream_framing_chunked)
        return true;   /* raw 分帧没有终止块（帧自带长度） */
    static const char k_last[] = "0\r\n\r\n";
    return tp_send(k_last, sizeof k_last - 1);
}

void stream_set_control_handler(stream_control_fn fn, void *ctx)
{
    g_ctrl_fn  = fn;
    g_ctrl_ctx = ctx;
}

int stream_poll_frame(unsigned *out_type, unsigned *out_flags, char *json, size_t json_cap)
{
    if (!g_active || g_tp.recv_nowait == nullptr)
        return 0;   /* 没有执行连接 / 传输不支持非阻塞读：安静地当作「没收到」 */

    /* ① 尽快把已有字节收进重组缓冲（最多补到满） */
    for (;;)
    {
        if (g_poll_have >= sizeof g_poll_buf)
            break;
        int r = g_tp.recv_nowait(g_tp.ctx, g_poll_buf + g_poll_have, sizeof g_poll_buf - g_poll_have);
        if (r > 0)
        {
            g_poll_have += (size_t)r;
            continue;
        }
        if (r == 0)
            return -1;   /* 对端关闭 */
        if (r == -2)
            break;       /* 现在没数据 */
        return -1;
    }

    /* ② 够一帧了吗（raw 分帧：4 字节长度前缀 + payload） */
    if (g_poll_have < 4)
        return 0;
    size_t payload = 0;
    if (!common::frame_prefix_decode(g_poll_buf, 4, &payload))
        return -2;
    if (payload > (size_t)k_ctrl_max)
        return -2;   /* 执行期收到大帧（比如一次新 script）= 协议错误，连接作废 */
    size_t total = 4 + payload;
    if (g_poll_have < total)
        return 0;    /* 还没收完 */

    common::frame_header fh;
    if (!common::frame_header_decode(g_poll_buf + 4, 8, &fh))
        return -2;
    size_t json_len = 0;
    if (!common::frame_json_len(payload, fh.data_len, &json_len))
        return -2;

    /* ③ 交出去（data 段忽略：control 的负载全在 flags 里） */
    if (out_type != nullptr)
        *out_type = fh.type;
    if (out_flags != nullptr)
        *out_flags = fh.flags;
    if (json != nullptr && json_cap > 1)
    {
        size_t n = (json_len < json_cap - 1) ? json_len : json_cap - 1;
        for (size_t i = 0; i < n; i++)
            json[i] = (char)g_poll_buf[4 + 8 + i];
        json[n] = '\0';
    }

    /* 把剩下的字节移到缓冲头（一次执行里控制帧很少，但别让它堆满） */
    size_t rest = g_poll_have - total;
    for (size_t i = 0; i < rest; i++)
        g_poll_buf[i] = g_poll_buf[total + i];
    g_poll_have = rest;
    return 1;
}

void stream_poll_and_dispatch()
{
    if (!g_active || g_ctrl_fn == nullptr)
        return;   /* 本地模式 / 没有执行连接：两个 poll 点必须接近零开销 */

    unsigned type  = 0;
    unsigned flags = 0;
    char     json[64];
    int      r     = stream_poll_frame(&type, &flags, json, sizeof json);
    if (r != 1)
        return;   /* 0 = 没帧；-1/-2 = 连接坏了（交给下一轮 guard 处理） */
    if (type == stream_frame_control)
        g_ctrl_fn(g_ctrl_ctx, flags, flags);   /* 命令就在 flags（见 stream_control_cmd） */
    else if (type == stream_frame_end)
        stream_end();                          /* 客户端主动收尾：后续写入全局失活 */
    /* 其它类型在执行期属于协议错误，忽略（不把执行打断） */
}
} /* namespace autodbg::host */
