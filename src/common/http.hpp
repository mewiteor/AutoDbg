#ifndef AUTODBG_COMMON_HTTP_HPP
#define AUTODBG_COMMON_HTTP_HPP
/* =============================================================================
 * src/common/http.hpp —— HTTP/1.1 子集（请求解析 + 响应生成，**不含 socket**）
 *
 * 来源：新写（plan_server.md S0 / §4.2 / §4.8，决策 D20）。
 * 职责：把 server 需要的 HTTP 部分做成**纯函数/纯对象**，与网络层分离：
 *   · 解析请求头（方法 / 目标 / 路径 / 查询串 / 关心的几个头 / Content-Length）；
 *   · 生成响应（状态行 + 头 + 体，v1 用 `Content-Length`；`chunked` 留 S6）；
 *   · 因为不含 socket，可以用表驱动自测（`src/common/selftest.cpp`）。
 * 明确不做（§4.2）：TLS、keep-alive、HTTP/2、pipelining、chunked **请求**体、
 *   multipart、请求体压缩、代理绝对 URI。
 * 约束：零 CRT + 无 STL；所有长度都有上限，越界一律报错而不是截断后继续。
 * ========================================================================== */
#include <stddef.h>

namespace autodbg::common
{
/* ------------------------------- 上限 ------------------------------- */
constexpr size_t   k_http_max_header_block = 8192; /* 请求头块字节上限（含结尾空行） */
constexpr unsigned k_http_max_headers      = 64;   /* 头数量上限 */
constexpr size_t   k_http_max_target       = 511;  /* request-target 上限 */
constexpr size_t   k_http_max_path         = 511;
constexpr size_t   k_http_max_query        = 511;
constexpr size_t   k_http_max_name         = 127;  /* 单个头值（filename/token 等）上限 */

enum class http_method : unsigned char
{
    get     = 0,
    post    = 1,
    head    = 2,
    options = 3,
    other   = 4,
};

enum class http_parse_status : unsigned char
{
    ok = 0,
    incomplete,  /* 数据还不够（调用方继续 recv） */
    bad_request, /* 语法/取值非法 → 400 */
    too_large,   /* 头块或头数量超限 → 413 */
    unsupported, /* 不支持的传输方式（chunked 请求体）→ 501 */
};

struct http_request
{
    http_method method = http_method::other;
    char        method_raw[8] = {};  /* "GET" / "POST" …（大写，含 NUL） */
    char        target[k_http_max_target + 1] = {}; /* 原始 request-target（含查询串） */
    char        path[k_http_max_path + 1]     = {}; /* 去掉查询串、已 %XX 解码 */
    char        query[k_http_max_query + 1]   = {}; /* 查询串原文（未解码；空串 = 无查询串） */
    char        version[16]                   = {}; /* "HTTP/1.1" */

    bool      expect_continue   = false;
    bool      keep_alive        = false;
    bool      chunked_request   = false;
    bool      has_content_length = false;
    long long content_length    = 0;

    char     content_type[k_http_max_name + 1] = {};
    char     filename[260]                     = {}; /* X-AutoDbg-Filename */
    char     format[16]                        = {}; /* X-AutoDbg-Format: json|text */
    char     token[k_http_max_name + 1]        = {}; /* Bearer 之后 / X-AutoDbg-Token */
    bool     has_token                         = false;

    /* S6（plan_server.md §8）：MCP 的会话头与流式协商 */
    char     mcp_session_id[80]                = {}; /* Mcp-Session-Id（没给 = 空串） */
    bool     wants_sse                         = false; /* Accept 里含 text/event-stream */
    unsigned header_count                      = 0;
};

/* 解析请求头（`buf/len` = 已收到字节，可多不可少；body 不计入）。
 * 成功时 `*head_len` = 头块字节数（body 从 `buf + *head_len` 开始）。
 * 失败时 `*head_len` 置 0，`out` 内容不保证。 */
http_parse_status http_parse_request(const char *buf, size_t len, http_request *out, size_t *head_len);

/* 查询串取第一个同名参数（`%XX` 与 `+` 都解码）；找到返回 true */
bool http_query_get(const char *query, const char *name, char *dst, size_t cap);

/* URL 解码（`%XX` → 字节、`+` → 空格）。返回写入长度；非法/越界返回 -1。 */
int http_url_decode(const char *src, size_t n, char *dst, size_t cap);

/* 状态码 → 原因短语（固定表；未知返回 "Unknown"） */
const char *http_reason(int status);

/* --------------------------- 响应生成器 ---------------------------
 * 三段式（§4.8）：构造（状态 + Content-Type）→ write()… → finish()。
 * `finish()` 之后调用方拿到「头 + 体」的连续字节；缓冲由对象持有，直到 reset/析构。
 * v1 只产出 `Content-Length` 形态（`Connection: close`）；S6 加了 `use_chunked(true)`
 * 形态（streamable HTTP / SSE 用），现有调用点不受影响。 */
class http_response
{
public:
    http_response(int status, const char *content_type) noexcept;
    ~http_response() noexcept;

    http_response(const http_response &) = delete;
    http_response &operator=(const http_response &) = delete;

    void reset(int status, const char *content_type) noexcept;

    /* 附加头（按加入顺序输出，位于 Content-Type/Content-Length/Connection 之前） */
    void add_header(const char *name, const char *value) noexcept;
    void add_header_number(const char *name, long long value) noexcept;

    void write(const void *data, size_t n) noexcept;
    void write(const char *s) noexcept;

    /* S6：chunked 形态（streamable HTTP / SSE）。
     * 目前 finish() 把整个 body 作为**单帧**写出 + 终止块（合法的 chunked 编码）；
     * 「边执行边推帧」的增量流式留待后续（见 plan_server.md §2.1）。 */
    void use_chunked(bool on) noexcept { m_chunked = on; }
    bool chunked() const noexcept { return m_chunked; }

    /* 生成完整响应；返回缓冲指针与长度。失败（内存不足）返回 nullptr。 */
    const char *finish(size_t *out_len) noexcept;

    bool overflow() const noexcept { return m_header_overflow || m_body_overflow; }
    int  status() const noexcept { return m_status; }

private:
    void put_header(const char *name, const char *value, size_t value_len) noexcept;

    char       *m_headers        = nullptr;
    size_t      m_headers_len    = 0;
    size_t      m_headers_cap    = 0;
    bool        m_header_overflow = false;

    char       *m_body           = nullptr;
    size_t      m_body_len       = 0;
    size_t      m_body_cap       = 0;
    bool        m_body_overflow  = false;

    char       *m_out            = nullptr;
    size_t      m_out_cap        = 0;

    int         m_status         = 200;
    const char *m_content_type   = "application/json; charset=utf-8";
    bool        m_chunked        = false;
};
} /* namespace autodbg::common */

#endif /* AUTODBG_COMMON_HTTP_HPP */
