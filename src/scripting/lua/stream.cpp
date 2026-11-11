/* =============================================================================
 * src/scripting/lua/stream.cpp —— 流式 `/run` 响应的帧写入（见 stream.hpp 的设计说明）
 *
 * 关键约束（为什么这么写）：
 *   · 帧由**执行脚本的那条线程**直接写 socket —— `f:write` 因此不需要 yield，回调里也能用；
 *   · 不缓冲整条流、不落盘：每帧发完就走，内存只占调用方给的那一块；
 *   · 背压交给 socket：`net_send_all` 带超时，客户端断了/太慢就返回失败（上层报错，不卡死）。
 * ========================================================================== */
#include "stream.hpp"

#include "common/net.hpp" /* net_send_all（已带 select 超时） */

#include <string.h>

namespace autodbg::host
{
namespace
{
long long g_sock     = -1;
unsigned  g_timeout  = 8000;
bool      g_active   = false;

void put_u16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
}

void put_u32(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
    p[2] = (unsigned char)((v >> 16) & 0xFFu);
    p[3] = (unsigned char)((v >> 24) & 0xFFu);
}

bool send_bytes(const void *p, size_t n)
{
    if (n == 0)
        return true;
    if (g_sock < 0)
        return false;
    return autodbg::common::net_send_all(g_sock, p, n, (int)g_timeout);
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
    g_active  = false;

    static const char k_head[] = "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: application/x-autodbg-stream\r\n"
                                 "Transfer-Encoding: chunked\r\n"
                                 "Connection: close\r\n"
                                 "X-AutoDbg-Stream: 1\r\n"
                                 "\r\n";
    if (!send_bytes(k_head, sizeof k_head - 1))
    {
        g_sock = -1;
        return false;
    }
    g_active = true;
    return true;
}

void stream_end()
{
    g_active = false;
    g_sock   = -1;
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
    if (!g_active || g_sock < 0)
        return false;

    unsigned char frame[8];
    frame[0] = (unsigned char)type;
    frame[1] = (unsigned char)flags;
    put_u16(frame + 2, (unsigned)id);
    put_u32(frame + 4, (unsigned)data_len);

    char   chunk_head[24];
    size_t payload = sizeof frame + json_len + data_len;
    size_t hn      = make_chunk_head(chunk_head, payload);

    static const char k_crlf[] = "\r\n";
    bool ok = send_bytes(chunk_head, hn) && send_bytes(frame, sizeof frame) &&
              send_bytes(json, json_len) && send_bytes(data, data_len) && send_bytes(k_crlf, 2);
    if (!ok)
    {
        /* 客户端断开/超时：标记流失效，后续 `f:write` 立刻拿到错误而不是反复等超时 */
        g_active = false;
    }
    return ok;
}

bool stream_finish()
{
    if (!g_active || g_sock < 0)
        return false;
    static const char k_last[] = "0\r\n\r\n";
    return send_bytes(k_last, sizeof k_last - 1);
}
} /* namespace autodbg::host */
