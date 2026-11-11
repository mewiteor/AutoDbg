/* =============================================================================
 * src/common/http.cpp —— HTTP/1.1 子集实现（请求解析 + 响应生成）
 *
 * 来源：新写（plan_server.md S0）。
 * 与旧实现的差异：无（新文件）。
 * 口径（都对应 §4.2 的「必须 / 不做」）：
 *   · 严格 CRLF 行尾；容忍请求行之前的空行（RFC 7230 §3.5 允许，浏览器有时会发）。
 *   · 拒绝 obs-fold（行首空格/制表符续行）—— 复杂的续行解析是走私攻击的温床，v1 不做。
 *   · `Content-Length` 重复且取值不同 → 400（避免请求走私）；`Transfer-Encoding: chunked`
 *     一律 501（v1 不支持 chunked 请求体）。
 *   · 所有拷贝都带上限，超限返回明确状态，**绝不**静默截断。
 * ========================================================================== */
#include "http.hpp"

#include "allocator.hpp"

#include <string.h> /* memcpy / memcmp / memchr / strlen（影子头 → msvcrt.dll） */

namespace autodbg::common
{
namespace
{
/* ------------------------------ 小工具 ------------------------------ */
inline char to_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* `a[an]`（小写化后）是否等于 NUL 结尾的小写 `b` */
bool ieq(const char *a, size_t an, const char *b)
{
    size_t i = 0;
    for (; i < an; i++)
    {
        if (b[i] == '\0')
            return false;
        if (to_lower(a[i]) != b[i])
            return false;
    }
    return b[i] == '\0';
}

/* `a[an]` 是否以 NUL 结尾的小写 `b` 开头（不区分大小写） */
bool istarts_with(const char *a, size_t an, const char *b)
{
    size_t i = 0;
    for (; b[i] != '\0'; i++)
    {
        if (i >= an || to_lower(a[i]) != b[i])
            return false;
    }
    return true;
}

bool is_ows(char c)
{
    return c == ' ' || c == '\t';
}

void trim_ows(const char **s, size_t *n)
{
    const char *p = *s;
    size_t      m = *n;
    while (m > 0 && is_ows(*p))
    {
        p++;
        m--;
    }
    while (m > 0 && is_ows(p[m - 1]))
        m--;
    *s = p;
    *n = m;
}

/* 带 NUL、带上限的拷贝；超限返回 false（不动 dst 之外的任何内存） */
bool copy_cap(char *dst, size_t cap, const char *src, size_t n)
{
    if (dst == nullptr || cap == 0)
        return false;
    if (n + 1 > cap)
    {
        dst[0] = '\0';
        return false;
    }
    if (n > 0)
        memcpy(dst, src, n);
    dst[n] = '\0';
    return true;
}

/* 严格十进制（不许符号/空白/前导 +）；越界返回 false */
bool parse_ll(const char *s, size_t n, long long *out)
{
    if (n == 0 || n > 18)
        return false;
    long long v = 0;
    for (size_t i = 0; i < n; i++)
    {
        if (s[i] < '0' || s[i] > '9')
            return false;
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return true;
}

int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* 找头块结束（"\r\n\r\n"）：返回其**之后**的偏移。找不到返回 0。 */
size_t find_head_end(const char *buf, size_t len)
{
    size_t start = 0;
    /* 允许请求行之前出现空行（RFC 7230 §3.5） */
    while (start + 1 < len && buf[start] == '\r' && buf[start + 1] == '\n')
        start += 2;

    for (size_t i = start; i + 3 < len; i++)
    {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return i + 4;
    }
    return 0;
}
} /* namespace */

/* ------------------------------ URL 解码 ------------------------------ */
int http_url_decode(const char *src, size_t n, char *dst, size_t cap)
{
    if (src == nullptr || dst == nullptr || cap == 0)
        return -1;

    size_t o = 0;
    for (size_t i = 0; i < n; i++)
    {
        char c = src[i];
        if (c == '+')
        {
            if (o + 1 >= cap)
                return -1;
            dst[o++] = ' ';
            continue;
        }
        if (c == '%')
        {
            if (i + 2 >= n) /* 需要 i+1、i+2 两个字节在场 */
                return -1;
            int hi = hexval(src[i + 1]);
            int lo = hexval(src[i + 2]);
            if (hi < 0 || lo < 0)
                return -1;
            if (o + 1 >= cap)
                return -1;
            dst[o++] = (char)((hi << 4) | lo);
            i += 2;
            continue;
        }
        if (o + 1 >= cap)
            return -1;
        dst[o++] = c;
    }
    dst[o] = '\0';
    return (int)o;
}

bool http_query_get(const char *query, const char *name, char *dst, size_t cap)
{
    if (query == nullptr || name == nullptr || dst == nullptr || cap == 0)
        return false;

    size_t name_len = strlen(name);
    size_t i        = 0;
    while (query[i] != '\0')
    {
        size_t start = i;
        while (query[i] != '\0' && query[i] != '&')
            i++;
        size_t seg_end = i; /* [start, seg_end) 是一段 */

        size_t eq = start;
        while (eq < seg_end && query[eq] != '=')
            eq++;

        if (eq - start == name_len && 0 == memcmp(query + start, name, name_len))
        {
            const char *v     = (eq < seg_end) ? (query + eq + 1) : "";
            size_t      v_len = (eq < seg_end) ? (seg_end - eq - 1) : 0;
            return http_url_decode(v, v_len, dst, cap) >= 0;
        }

        if (query[i] == '&')
            i++;
    }
    return false;
}

/* ------------------------------ 请求解析 ------------------------------ */
http_parse_status http_parse_request(const char *buf, size_t len, http_request *out, size_t *head_len)
{
    if (head_len != nullptr)
        *head_len = 0;
    if (out == nullptr)
        return http_parse_status::bad_request;
    if (buf == nullptr)
        return (len == 0) ? http_parse_status::incomplete : http_parse_status::bad_request;

    size_t head = find_head_end(buf, len);
    if (head == 0)
        return (len >= k_http_max_header_block) ? http_parse_status::too_large : http_parse_status::incomplete;
    if (head > k_http_max_header_block)
        return http_parse_status::too_large;

    /* 跳过前导空行后再取请求行 */
    size_t pos = 0;
    while (pos + 1 < head && buf[pos] == '\r' && buf[pos + 1] == '\n')
        pos += 2;

    /* ---- 请求行 ---- */
    size_t line_end = pos;
    while (line_end + 1 < head && !(buf[line_end] == '\r' && buf[line_end + 1] == '\n'))
        line_end++;
    if (line_end + 1 >= head)
        return http_parse_status::bad_request;

    size_t p = pos;
    size_t m_start = p;
    while (p < line_end && buf[p] != ' ')
        p++;
    if (p >= line_end || p == m_start || p - m_start > 7)
        return http_parse_status::bad_request;
    for (size_t i = m_start; i < p; i++)
    {
        char c = buf[i];
        if (c < 'A' || c > 'Z')
            return http_parse_status::bad_request;
    }
    copy_cap(out->method_raw, sizeof out->method_raw, buf + m_start, p - m_start);

    while (p < line_end && buf[p] == ' ')
        p++;
    size_t t_start = p;
    while (p < line_end && buf[p] != ' ')
        p++;
    if (p == t_start || p - t_start > k_http_max_target)
        return http_parse_status::bad_request;
    if (!copy_cap(out->target, sizeof out->target, buf + t_start, p - t_start))
        return http_parse_status::bad_request;

    while (p < line_end && buf[p] == ' ')
        p++;
    if (p >= line_end || line_end - p > sizeof out->version - 1)
        return http_parse_status::bad_request;
    if (!istarts_with(buf + p, line_end - p, "http/1."))
        return http_parse_status::bad_request;
    copy_cap(out->version, sizeof out->version, buf + p, line_end - p);

    /* 方法映射 */
    if (ieq(out->method_raw, strlen(out->method_raw), "get"))
        out->method = http_method::get;
    else if (ieq(out->method_raw, strlen(out->method_raw), "post"))
        out->method = http_method::post;
    else if (ieq(out->method_raw, strlen(out->method_raw), "head"))
        out->method = http_method::head;
    else if (ieq(out->method_raw, strlen(out->method_raw), "options"))
        out->method = http_method::options;
    else
        out->method = http_method::other;

    /* ---- target → path + query ---- */
    const char *target = out->target;
    size_t      tlen   = strlen(target);
    size_t      qmark  = tlen;
    for (size_t i = 0; i < tlen; i++)
    {
        if (target[i] == '?')
        {
            qmark = i;
            break;
        }
    }
    if (qmark == 0 || target[0] != '/')
        return http_parse_status::bad_request;
    if (http_url_decode(target, qmark, out->path, sizeof out->path) < 0)
        return http_parse_status::bad_request;
    if (qmark < tlen)
    {
        size_t qlen = tlen - qmark - 1;
        if (qlen > k_http_max_query)
            return http_parse_status::bad_request;
        if (!copy_cap(out->query, sizeof out->query, target + qmark + 1, qlen))
            return http_parse_status::bad_request;
    }
    else
    {
        out->query[0] = '\0';
    }

    /* ---- 头 ---- */
    pos = line_end + 2;
    while (pos + 1 < head)
    {
        /* 空行 = 头块结束 */
        if (buf[pos] == '\r' && buf[pos + 1] == '\n')
            break;

        size_t end = pos;
        while (end + 1 < head && !(buf[end] == '\r' && buf[end + 1] == '\n'))
            end++;

        if (is_ows(buf[pos]))
            return http_parse_status::bad_request; /* obs-fold：v1 明确拒绝 */

        size_t colon = pos;
        while (colon < end && buf[colon] != ':')
            colon++;
        if (colon >= end || colon == pos)
            return http_parse_status::bad_request;

        const char *name = buf + pos;
        size_t      nlen = colon - pos;
        const char *val  = buf + colon + 1;
        size_t      vlen = end - colon - 1;
        trim_ows(&name, &nlen);
        trim_ows(&val, &vlen);

        out->header_count++;
        if (out->header_count > k_http_max_headers)
            return http_parse_status::too_large;

        if (ieq(name, nlen, "content-length"))
        {
            long long cl = 0;
            if (!parse_ll(val, vlen, &cl))
                return http_parse_status::bad_request;
            if (out->has_content_length && out->content_length != cl)
                return http_parse_status::bad_request; /* 走私防护 */
            out->has_content_length = true;
            out->content_length     = cl;
        }
        else if (ieq(name, nlen, "transfer-encoding"))
        {
            /* 只要出现 chunked 就整条拒绝（v1 不支持） */
            for (size_t i = 0; i + 7 <= vlen; i++)
            {
                if (istarts_with(val + i, vlen - i, "chunked"))
                    out->chunked_request = true;
            }
        }
        else if (ieq(name, nlen, "expect"))
        {
            if (istarts_with(val, vlen, "100-continue"))
                out->expect_continue = true;
        }
        else if (ieq(name, nlen, "connection"))
        {
            if (istarts_with(val, vlen, "close"))
                out->keep_alive = false;
            else if (istarts_with(val, vlen, "keep-alive"))
                out->keep_alive = true;
        }
        else if (ieq(name, nlen, "content-type"))
        {
            if (!copy_cap(out->content_type, sizeof out->content_type, val, vlen))
                return http_parse_status::too_large;
        }
        else if (ieq(name, nlen, "x-autodbg-filename"))
        {
            if (!copy_cap(out->filename, sizeof out->filename, val, vlen))
                return http_parse_status::too_large;
        }
        else if (ieq(name, nlen, "x-autodbg-format"))
        {
            if (!copy_cap(out->format, sizeof out->format, val, vlen))
                return http_parse_status::too_large;
            for (char *q = out->format; *q != '\0'; q++)
                *q = to_lower(*q);
        }
        else if (ieq(name, nlen, "x-autodbg-token"))
        {
            if (!copy_cap(out->token, sizeof out->token, val, vlen))
                return http_parse_status::too_large;
            out->has_token = true;
        }
        else if (ieq(name, nlen, "x-autodbg-stream"))
        {
            /* 流式 /run（帧 + chunked）：值“1”/“true”/“yes” 都算开；“0” 明确关 */
            out->wants_stream = !(vlen == 1 && val[0] == '0');
        }
        else if (ieq(name, nlen, "authorization"))
        {
            if (istarts_with(val, vlen, "bearer "))
            {
                const char *t  = val + 7;
                size_t      tn = vlen - 7;
                trim_ows(&t, &tn);
                if (!copy_cap(out->token, sizeof out->token, t, tn))
                    return http_parse_status::too_large;
                out->has_token = true;
            }
        }
        else if (ieq(name, nlen, "mcp-session-id"))
        {
            if (!copy_cap(out->mcp_session_id, sizeof out->mcp_session_id, val, vlen))
                return http_parse_status::too_large;
        }
        else if (ieq(name, nlen, "accept"))
        {
            /* 只看「要不要 SSE / 帧流」，不需要完整 Accept 协商 */
            for (size_t i = 0; i + 17 <= vlen; i++)
            {
                if (istarts_with(val + i, vlen - i, "text/event-stream"))
                {
                    out->wants_sse = true;
                    break;
                }
            }
            for (size_t i = 0; i + 25 <= vlen; i++)
            {
                if (istarts_with(val + i, vlen - i, "application/x-autodbg-stream"))
                {
                    out->wants_stream = true;
                    break;
                }
            }
        }

        pos = end + 2;
    }

    if (out->chunked_request)
        return http_parse_status::unsupported;

    if (head_len != nullptr)
        *head_len = head;
    return http_parse_status::ok;
}

/* ------------------------------ 状态码 ------------------------------ */
const char *http_reason(int status)
{
    switch (status)
    {
    case 100: return "Continue";
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 415: return "Unsupported Media Type";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default:  return "Unknown";
    }
}

/* ------------------------------ 响应 ------------------------------ */
namespace
{
void append(char **buf, size_t *len, size_t *cap, const void *data, size_t n, bool *overflow)
{
    if (n == 0)
        return;
    if (*overflow)
        return;

    if (*cap == 0)
    {
        size_t c = (n + 64 < 256) ? 256 : n + 64;
        char  *p = (char *)alloc(c);
        if (p == nullptr)
        {
            *overflow = true;
            return;
        }
        *buf = p;
        *cap = c;
        *len = 0;
    }
    if (*len + n + 1 > *cap)
    {
        size_t c = *cap;
        while (c < *len + n + 1)
        {
            size_t next = c + c / 2;
            if (next <= c)
            {
                *overflow = true;
                return;
            }
            c = next;
        }
        char *p = (char *)resize(*buf, c);
        if (p == nullptr)
        {
            *overflow = true;
            return;
        }
        *buf = p;
        *cap = c;
    }
    memcpy(*buf + *len, data, n);
    *len += n;
    (*buf)[*len] = '\0';
}

void append_str(char **buf, size_t *len, size_t *cap, const char *s, bool *overflow)
{
    if (s == nullptr)
        return;
    append(buf, len, cap, s, strlen(s), overflow);
}

void append_num(char **buf, size_t *len, size_t *cap, long long v, bool *overflow)
{
    char tmp[24];
    int  i   = 0;
    bool neg = (v < 0);
    unsigned long long u = neg ? (unsigned long long)(-(v + 1)) + 1ULL : (unsigned long long)v;
    do
    {
        tmp[i++] = (char)('0' + (int)(u % 10));
        u /= 10;
    } while (u != 0);
    if (neg)
        tmp[i++] = '-';
    char out[24];
    int  o = 0;
    while (i > 0)
        out[o++] = tmp[--i];
    append(buf, len, cap, out, (size_t)o, overflow);
}
} /* namespace */

http_response::http_response(int status, const char *content_type) noexcept
{
    m_status       = status;
    m_content_type = (content_type != nullptr) ? content_type : "application/json; charset=utf-8";
}

http_response::~http_response() noexcept
{
    if (m_headers != nullptr)
        release(m_headers);
    if (m_body != nullptr)
        release(m_body);
    if (m_out != nullptr)
        release(m_out);
}

void http_response::reset(int status, const char *content_type) noexcept
{
    m_status         = status;
    m_content_type   = (content_type != nullptr) ? content_type : "application/json; charset=utf-8";
    m_chunked        = false;
    m_headers_len    = 0;
    m_body_len       = 0;
    m_header_overflow = false;
    m_body_overflow   = false;
    if (m_headers != nullptr)
        m_headers[0] = '\0';
    if (m_body != nullptr)
        m_body[0] = '\0';
}

void http_response::put_header(const char *name, const char *value, size_t value_len) noexcept
{
    append_str(&m_headers, &m_headers_len, &m_headers_cap, name, &m_header_overflow);
    append_str(&m_headers, &m_headers_len, &m_headers_cap, ": ", &m_header_overflow);
    append(&m_headers, &m_headers_len, &m_headers_cap, value, value_len, &m_header_overflow);
    append_str(&m_headers, &m_headers_len, &m_headers_cap, "\r\n", &m_header_overflow);
}

void http_response::add_header(const char *name, const char *value) noexcept
{
    if (name == nullptr)
        return;
    put_header(name, (value != nullptr) ? value : "", (value != nullptr) ? strlen(value) : 0);
}

void http_response::add_header_number(const char *name, long long value) noexcept
{
    if (name == nullptr)
        return;
    char tmp[24];
    int  i   = 0;
    bool neg = (value < 0);
    unsigned long long u = neg ? (unsigned long long)(-(value + 1)) + 1ULL : (unsigned long long)value;
    do
    {
        tmp[i++] = (char)('0' + (int)(u % 10));
        u /= 10;
    } while (u != 0);
    if (neg)
        tmp[i++] = '-';
    char out[24];
    int  o = 0;
    while (i > 0)
        out[o++] = tmp[--i];
    put_header(name, out, (size_t)o);
}

void http_response::write(const void *data, size_t n) noexcept
{
    append(&m_body, &m_body_len, &m_body_cap, data, n, &m_body_overflow);
}

void http_response::write(const char *s) noexcept
{
    if (s == nullptr)
        return;
    append_str(&m_body, &m_body_len, &m_body_cap, s, &m_body_overflow);
}

const char *http_response::finish(size_t *out_len) noexcept
{
    if (out_len != nullptr)
        *out_len = 0;
    if (m_out != nullptr)
    {
        release(m_out);
        m_out    = nullptr;
        m_out_cap = 0;
    }

    size_t len = 0;
    size_t cap = 0;
    bool   ovf = false;

    append_str(&m_out, &len, &cap, "HTTP/1.1 ", &ovf);
    append_num(&m_out, &len, &cap, (long long)m_status, &ovf);
    append_str(&m_out, &len, &cap, " ", &ovf);
    append_str(&m_out, &len, &cap, http_reason(m_status), &ovf);
    append_str(&m_out, &len, &cap, "\r\n", &ovf);

    /* 调用方附加的头（顺序稳定，便于逐字节断言） */
    if (m_headers != nullptr && m_headers_len > 0)
        append(&m_out, &len, &cap, m_headers, m_headers_len, &ovf);

    append_str(&m_out, &len, &cap, "Content-Type: ", &ovf);
    append_str(&m_out, &len, &cap, m_content_type, &ovf);

    if (m_chunked)
    {
        /* S6：chunked 形态（streamable HTTP / SSE）。整个 body 作为单帧 + 终止块。 */
        append_str(&m_out, &len, &cap, "\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n", &ovf);
        if (m_body_len > 0)
        {
            char head[24];
            int  i = 0;
            unsigned long long v = (unsigned long long)m_body_len;
            do
            {
                head[i++] = "0123456789abcdef"[v & 0xF];
                v >>= 4;
            } while (v != 0);
            while (i > 0)
            {
                char one[2] = { head[--i], '\0' };
                append_str(&m_out, &len, &cap, one, &ovf);
            }
            append_str(&m_out, &len, &cap, "\r\n", &ovf);
            append(&m_out, &len, &cap, m_body, m_body_len, &ovf);
            append_str(&m_out, &len, &cap, "\r\n", &ovf);
        }
        append_str(&m_out, &len, &cap, "0\r\n\r\n", &ovf);
    }
    else
    {
        append_str(&m_out, &len, &cap, "\r\nContent-Length: ", &ovf);
        append_num(&m_out, &len, &cap, (long long)m_body_len, &ovf);
        append_str(&m_out, &len, &cap, "\r\nConnection: close\r\n\r\n", &ovf);

        if (m_body != nullptr && m_body_len > 0)
            append(&m_out, &len, &cap, m_body, m_body_len, &ovf);
    }

    if (ovf || m_out == nullptr)
        return nullptr;
    if (out_len != nullptr)
        *out_len = len;
    return m_out;
}
} /* namespace autodbg::common */
