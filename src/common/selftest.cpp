/* =============================================================================
 * src/common/selftest.cpp —— 纯函数自测实现（plan_server.md S0）
 *
 * 来源：新写。
 * 与旧实现的差异：无（新文件）。
 * 覆盖：HTTP 请求解析（含各种拒绝路径）、HTTP 响应生成、json_writer（转义/嵌套/数字/
 *   深度溢出、**T1（D50）的全转义出口**）、base64（向量/往返/非法输入）、rand（hex 形状与
 *   两次不相等）、**帧头编解码（T2：定长头 / json 长度反推 / raw 长度前缀 / 全部拒绝路径）**。
 * 说明：断言用 `strcmp`/`memcmp` 逐字节比较（跨架构/跨编译器稳定）；
 *   期望值都是**手写的字面量**，不要在这里用被测代码生成期望值（否则等于没测）。
 * ========================================================================== */
#include "selftest.hpp"

#include "base64.hpp"
#include "frame.hpp"   /* T2：帧头编解码（plan_cli.md §2.1） */
#include "http.hpp"
#include "json_writer.hpp"
#include "png_writer.hpp"
#include "rand.hpp"

#include "allocator.hpp" /* release：截图编码的测试直接释放缓冲 */

#include <stdlib.h> /* strtod：double 的 round-trip 断言用 */
#include <string.h>

namespace autodbg::common
{
namespace
{
struct test_ctx
{
    selftest_report_fn report;
    void              *ctx;
    unsigned           total;
    unsigned           failed;
};

void say(test_ctx *t, const char *ok_flag, const char *name, const char *detail)
{
    char line[256];
    size_t n = 0;
    const char *parts[3] = {ok_flag, name, detail};

    for (int i = 0; i < 3; i++)
    {
        const char *p = parts[i];
        if (p == nullptr)
            continue;
        for (; *p != '\0' && n + 1 < sizeof line; p++)
            line[n++] = *p;
    }
    line[n] = '\0';
    if (t->report != nullptr)
        t->report(t->ctx, line);
}

void pass(test_ctx *t, const char *name)
{
    t->total++;
    say(t, "[ok] ", name, nullptr);
}

/* 往行缓冲追加一段文本（最多 max_chars 字符；不越界） */
void append_cap(char *dst, size_t cap, size_t *n, const char *s, size_t max_chars)
{
    size_t k = 0;
    if (s == nullptr)
        return;
    for (; *s != '\0' && k < max_chars && *n + 1 < cap; s++, k++)
        dst[(*n)++] = *s;
    dst[*n] = '\0';
}

/* 追加 `label<十进制>` */
void append_ll(char *dst, size_t cap, size_t *n, const char *label, long long v)
{
    append_cap(dst, cap, n, label, 16);

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
        dst[(*n)++] = '-';
    while (i > 0 && *n + 1 < cap)
        dst[(*n)++] = tmp[--i];
    dst[*n] = '\0';
}

void fail(test_ctx *t, const char *name, const char *detail)
{
    t->total++;
    t->failed++;
    say(t, "[FAIL] ", name, detail);
}

void check_bool(test_ctx *t, const char *name, bool cond, const char *detail)
{
    if (cond)
        pass(t, name);
    else
        fail(t, name, detail);
}

void check_int(test_ctx *t, const char *name, long long got, long long want, const char *detail)
{
    if (got == want)
    {
        pass(t, name);
        return;
    }
    /* 失败详情拼成 `got=? want=?`（不用 CRT 格式化） */
    char   buf[192];
    size_t n = 0;
    const char *prefix = (detail != nullptr) ? detail : "";
    for (const char *p = prefix; *p != '\0' && n + 1 < sizeof buf; p++)
        buf[n++] = *p;
    buf[n++] = ' ';
    buf[n]   = '\0';
    append_ll(buf, sizeof buf, &n, "got=", got);
    append_ll(buf, sizeof buf, &n, " want=", want);
    buf[n] = '\0';
    fail(t, name, buf);
}

void check_str(test_ctx *t, const char *name, const char *got, const char *want)
{
    if (got != nullptr && want != nullptr && strcmp(got, want) == 0)
    {
        pass(t, name);
        return;
    }
    char   buf[256];
    size_t n = 0;
    append_cap(buf, sizeof buf, &n, "got=[", 64);
    append_cap(buf, sizeof buf, &n, (got != nullptr) ? got : "(null)", 80);
    append_cap(buf, sizeof buf, &n, "] want=[", 64);
    append_cap(buf, sizeof buf, &n, (want != nullptr) ? want : "(null)", 80);
    append_cap(buf, sizeof buf, &n, "]", 64);
    buf[n] = '\0';
    fail(t, name, buf);
}

/* ------------------------------ HTTP 解析 ------------------------------ */
void test_http_parse(test_ctx *t)
{
    {
        const char *req = "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
        http_request r{};
        size_t       head = 0;
        bool ok = http_parse_request(req, strlen(req), &r, &head) == http_parse_status::ok;
        check_bool(t, "http.parse.get-minimal", ok, "expected ok");
        check_int(t, "http.parse.get-minimal.method", (long long)r.method, (long long)http_method::get, nullptr);
        check_str(t, "http.parse.get-minimal.path", r.path, "/health");
        check_int(t, "http.parse.get-minimal.head-len", (long long)head, (long long)strlen(req), nullptr);
    }

    {
        const char *req = "POST /run?format=json&args=a&args=b HTTP/1.1\r\n"
                          "Content-Type: text/x-lua\r\n"
                          "X-AutoDbg-Filename: buffer.lua\r\n"
                          "X-AutoDbg-Format: Text\r\n"
                          "Authorization: Bearer abc123\r\n"
                          "Expect: 100-continue\r\n"
                          "Content-Length: 5\r\n"
                          "\r\n"
                          "hello";
        http_request r{};
        size_t       head = 0;
        bool ok = http_parse_request(req, strlen(req), &r, &head) == http_parse_status::ok;
        check_bool(t, "http.parse.post-headers", ok, "expected ok");
        check_int(t, "http.parse.post-headers.method", (long long)r.method, (long long)http_method::post, nullptr);
        check_str(t, "http.parse.post-headers.path", r.path, "/run");
        check_str(t, "http.parse.post-headers.query", r.query, "format=json&args=a&args=b");
        check_str(t, "http.parse.post-headers.filename", r.filename, "buffer.lua");
        check_str(t, "http.parse.post-headers.format-lowercased", r.format, "text");
        check_str(t, "http.parse.post-headers.token", r.token, "abc123");
        check_bool(t, "http.parse.post-headers.expect-continue", r.expect_continue, "expected true");
        check_int(t, "http.parse.post-headers.content-length", r.content_length, 5, nullptr);
        check_int(t, "http.parse.post-headers.body-offset",
                  (long long)(strlen(req) - head), 5, "body should be 5 bytes");
    }

    {
        char        dst[32];
        bool        found = http_query_get("b=%20x+y", "b", dst, sizeof dst);
        check_bool(t, "http.parse.query-decode", found && strcmp(dst, " x y") == 0, "expected ' x y'");
    }

    {
        /* 严格 CRLF：只有 LF 的头块永远等不到结束标记 → incomplete */
        const char *req = "GET / HTTP/1.0\n\n";
        http_request r{};
        size_t       head = 0;
        check_bool(t, "http.parse.lf-only-incomplete",
                   http_parse_request(req, strlen(req), &r, &head) == http_parse_status::incomplete,
                   "LF-only must not parse");
    }

    {
        /* 头部还一个字节都没收到时必须是 incomplete（S1 实测过的坑：空缓冲曾被判成 400） */
        http_request r{};
        size_t       head = 0;
        check_bool(t, "http.parse.empty-incomplete",
                   http_parse_request(nullptr, 0, &r, &head) == http_parse_status::incomplete, nullptr);
    }

    {
        const char *req = "GET /health HTT";
        http_request r{};
        size_t       head = 0;
        check_bool(t, "http.parse.incomplete",
                   http_parse_request(req, strlen(req), &r, &head) == http_parse_status::incomplete, nullptr);
    }

    {
        const char *req = "GARBAGE\r\n\r\n";
        http_request r{};
        size_t       head = 0;
        check_bool(t, "http.parse.bad-request-line",
                   http_parse_request(req, strlen(req), &r, &head) == http_parse_status::bad_request, nullptr);
    }

    {
        const char *req = "GET / HTTP/2.0\r\n\r\n";
        http_request r{};
        size_t       head = 0;
        check_bool(t, "http.parse.bad-version",
                   http_parse_request(req, strlen(req), &r, &head) == http_parse_status::bad_request, nullptr);
    }

    {
        const char *req = "GET foo HTTP/1.1\r\n\r\n";
        http_request r{};
        size_t       head = 0;
        check_bool(t, "http.parse.target-needs-slash",
                   http_parse_request(req, strlen(req), &r, &head) == http_parse_status::bad_request, nullptr);
    }

    {
        const char *req = "GET / HTTP/1.1\r\nA: 1\r\nB: 2\r\n\r\n";
        const char *fold = "GET / HTTP/1.1\r\nA: 1\r\n : 2\r\n\r\n";
        http_request r{};
        size_t       head = 0;
        (void)req;
        check_bool(t, "http.parse.obs-fold-rejected",
                   http_parse_request(fold, strlen(fold), &r, &head) == http_parse_status::bad_request, nullptr);
    }

    {
        const char *req = "POST /run HTTP/1.1\r\nContent-Length: 0\r\nContent-Length: 5\r\n\r\n";
        http_request r{};
        size_t       head = 0;
        check_bool(t, "http.parse.dup-content-length",
                   http_parse_request(req, strlen(req), &r, &head) == http_parse_status::bad_request, nullptr);
    }

    {
        const char *req = "POST /run HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";
        http_request r{};
        size_t       head = 0;
        check_bool(t, "http.parse.chunked-unsupported",
                   http_parse_request(req, strlen(req), &r, &head) == http_parse_status::unsupported, nullptr);
    }

    {
        /* 65 个头 → too_large */
        char buf[2048];
        size_t n = 0;
        const char *first = "GET / HTTP/1.1\r\n";
        for (const char *p = first; *p != '\0'; p++)
            buf[n++] = *p;
        for (int i = 0; i < k_http_max_headers + 1; i++)
        {
            const char *h = "X: 1\r\n";
            for (const char *p = h; *p != '\0'; p++)
                buf[n++] = *p;
        }
        buf[n++] = '\r';
        buf[n++] = '\n';
        http_request r{};
        size_t       head = 0;
        check_bool(t, "http.parse.too-many-headers",
                   http_parse_request(buf, n, &r, &head) == http_parse_status::too_large, nullptr);
    }

    {
        /* 没有结束标记且已超上限 → too_large（不能让对方无限发头） */
        static char buf[k_http_max_header_block + 16];
        size_t      n = 0;
        const char *first = "GET / HTTP/1.1\r\nX: ";
        for (const char *p = first; *p != '\0'; p++)
            buf[n++] = *p;
        while (n < sizeof buf)
            buf[n++] = 'a';
        http_request r{};
        size_t       head = 0;
        check_bool(t, "http.parse.header-block-too-large",
                   http_parse_request(buf, n, &r, &head) == http_parse_status::too_large, nullptr);
    }
}

/* ------------------------------ HTTP 响应 ------------------------------ */
void test_http_response(test_ctx *t)
{
    {
        http_response resp(200, "text/plain; charset=utf-8");
        resp.add_header("X-AutoDbg-Exit", "0");
        resp.write("hello", 5);

        size_t      len = 0;
        const char *raw = resp.finish(&len);
        const char *want = "HTTP/1.1 200 OK\r\n"
                           "X-AutoDbg-Exit: 0\r\n"
                           "Content-Type: text/plain; charset=utf-8\r\n"
                           "Content-Length: 5\r\n"
                           "Connection: close\r\n"
                           "\r\n"
                           "hello";
        check_bool(t, "http.response.byte-identical",
                   raw != nullptr && len == strlen(want) && memcmp(raw, want, len) == 0, "content mismatch");
    }

    {
        http_response resp(504, nullptr);
        size_t        len = 0;
        const char   *raw = resp.finish(&len);
        bool ok = raw != nullptr && len > 13 && memcmp(raw, "HTTP/1.1 504 ", 13) == 0 &&
                  resp.status() == 504;
        check_bool(t, "http.response.reason", ok, "504 reason line");
    }

    {
        http_response resp(413, nullptr);
        resp.add_header_number("Content-Length", 42);
        size_t len = 0;
        (void)resp.finish(&len);
        check_bool(t, "http.response.header-number", len > 0, "finish failed");
    }
}

/* ------------------------------ JSON ------------------------------ */
void test_json(test_ctx *t)
{
    {
        json_writer w;
        w.begin_object();
        w.key("a");
        w.begin_array();
        w.number(1LL);
        w.number(2LL);
        w.end_array();
        w.key("b");
        w.begin_object();
        w.key("c");
        w.string("x");
        w.end_object();
        w.end_object();
        check_str(t, "json.nested", w.data(), "{\"a\":[1,2],\"b\":{\"c\":\"x\"}}");
        check_bool(t, "json.nested.no-overflow", !w.overflow(), nullptr);
    }

    {
        json_writer w;
        w.begin_object();
        w.key("e");
        w.begin_object();
        w.end_object();
        w.key("f");
        w.begin_array();
        w.end_array();
        w.end_object();
        check_str(t, "json.empty-containers", w.data(), "{\"e\":{},\"f\":[]}");
    }

    {
        json_writer w;
        w.string("\"");
        check_str(t, "json.escape.quote", w.data(), "\"\\\"\"");

        w.reset();
        w.string("\\");
        check_str(t, "json.escape.backslash", w.data(), "\"\\\\\"");

        w.reset();
        w.string("\t");
        check_str(t, "json.escape.tab", w.data(), "\"\\t\"");

        w.reset();
        const char ctl[1] = {1};
        w.string(ctl, 1);
        check_str(t, "json.escape.control", w.data(), "\"\\u0001\"");

        w.reset();
        w.string("汉");
        check_str(t, "json.escape.utf8-passthrough", w.data(), "\"汉\"");
    }

    /* ---- T1（D50）：全转义出口 `string_escaped` / `raw_escaped`（输出必须纯 ASCII）----
     * 期望值是手写字面量；`\u6c49` 在两个变体里都成立：modern 的源字面量是 UTF-8（走
     * UTF-8 解码），legacy 的执行字符集是 GBK（走 CP936 分支）——两边结果同一。 */
    {
        json_writer w;
        w.string_escaped("");
        check_str(t, "json.esc.empty", w.data(), "\"\"");

        w.reset();
        w.string_escaped("a\"b\\c\td");
        check_str(t, "json.esc.short-escapes", w.data(), "\"a\\\"b\\\\c\\td\"");

        w.reset();
        w.string_escaped("汉");
        check_str(t, "json.esc.utf8-codepoint", w.data(), "\"\\u6c49\"");

        w.reset();
        const char astral[4] = {(char)0xF0, (char)0x9F, (char)0x98, (char)0x80}; /* U+1F600 */
        w.string_escaped(astral, 4);
        check_str(t, "json.esc.astral-surrogate-pair", w.data(), "\"\\ud83d\\ude00\"");

        /* 非法 UTF-8（GBK 字节串）：跨环境唯一不变的性质是「纯 ASCII」（CP936 分支或逐字节降级
         * 都只能写 \uXXXX）。具体码点取决于能否转换 → 不断言具体值。 */
        w.reset();
        const char gbk[2] = {(char)0xB0, (char)0xA1};
        w.string_escaped(gbk, 2);
        bool all_ascii = (w.data() != nullptr);
        for (const char *p = w.data(); p != nullptr && *p != '\0'; p++)
            if ((unsigned char)*p >= 0x80u)
                all_ascii = false;
        check_bool(t, "json.esc.high-bytes-escaped-as-ascii", all_ascii, w.data());
        check_bool(t, "json.esc.gbk-run-escapes-lossless",
                   w.size() >= 8 && w.size() <= 14, "一个 CP936 字符（1 个 \\u）或逐字节（2 个 \\u）");

        /* 纯 ASCII 输入 → 与既有 `string()` 逐字节相同（全转义出口只多改了非 ASCII） */
        w.reset();
        w.string("x\n");
        const char *want_plain = w.data();
        char cmp[64];
        size_t cn = 0;
        for (const char *p = want_plain; *p != '\0' && cn + 1 < sizeof cmp; p++)
            cmp[cn++] = *p;
        cmp[cn] = '\0';
        w.reset();
        w.string_escaped("x\n");
        check_str(t, "json.esc.ascii-identical-to-string", w.data(), cmp);

        /* 嵌套 + raw 片段（`result` 的路径：C 侧不写 parser，片段直通） */
        w.reset();
        w.begin_object();
        w.key("stdout");
        w.string_escaped("b\tc");
        w.key("result");
        w.raw_escaped("{\"k\":1}", 7);
        w.end_object();
        check_str(t, "json.esc.nested-raw", w.data(), "{\"stdout\":\"b\\tc\",\"result\":{\"k\":1}}");

        /* raw 片段里的非 ASCII（jsonx.encode 可能交回 GBK/UTF-8 字节）也被转义 */
        w.reset();
        const char frag[5] = {'"', (char)0xE6, (char)0xB1, (char)0x89, '"'};
        w.raw_escaped(frag, 5);
        check_str(t, "json.esc.raw-fragment-escaped", w.data(), "\"\\u6c49\"");

        /* 空片段 / NULL → null（信封里 `result` 编不了也要占位，不能省字段） */
        w.reset();
        w.raw_escaped(nullptr, 0);
        check_str(t, "json.esc.raw-null", w.data(), "null");

        /* double 的口径不变（Ryu：最短且可精确回读），与全转义字符串混排也照旧 */
        w.reset();
        w.begin_object();
        w.key("d");
        w.string_escaped("汉");
        w.key("n");
        w.number(0.5);
        w.end_object();
        check_str(t, "json.esc.double-ryu-unchanged", w.data(), "{\"d\":\"\\u6c49\",\"n\":0.5}");
    }

    {
        json_writer w;
        w.number(-1234567890123LL);
        check_str(t, "json.number.negative-int64", w.data(), "-1234567890123");
        w.reset();
        w.number(0.5);
        check_str(t, "json.number.half", w.data(), "0.5");

        w.reset();
        w.number(2.0);
        check_str(t, "json.number.integral-double", w.data(), "2");

        /* 有限的大数：具体字面量由 Ryu 的格式选择决定（如 1e+15），但必须是数字而不是 null */
        w.reset();
        w.number(1.0e15);
        check_bool(t, "json.number.big-finite", w.data() != nullptr && w.data()[0] != 'n', w.data());

        /* inf / nan → null（JSON 没有对应字面量） */
        volatile double zero = 0.0; /* 挡住常量折叠：1.0/0.0 是编译期错误 C2124 */
        w.reset();
        w.number(1.0 / zero);
        check_str(t, "json.number.inf-null", w.data(), "null");

        w.reset();
        w.number(zero / zero);
        check_str(t, "json.number.nan-null", w.data(), "null");
    }

    /* 关键性质（本轮要求）：dbl2str → 字符串 → strtod → **二进制不变** */
    {
        static const double k_values[] = {
            0.1,          0.5,           1.0 / 3.0,      -1.5,
            1.0e-300,     1.0e300,       123456789.123456789,
            3.14159265358979323846,       2.2250738585072014e-308,
            1.7976931348623157e308,       9007199254740993.0,   -0.0,
        };
        bool all_ok = true;
        for (size_t i = 0; i < sizeof k_values / sizeof k_values[0]; i++)
        {
            double      v = k_values[i];
            json_writer w;
            w.number(v);
            double back = strtod(w.data(), nullptr);
            if (memcmp(&back, &v, sizeof v) != 0)
                all_ok = false;
        }
        check_bool(t, "json.number.roundtrip-bits", all_ok, "有个 double 回读后二进制变了");
    }

    {
        json_writer w;
        for (int i = 0; i < 20; i++)
            w.begin_object();
        check_bool(t, "json.depth-overflow", w.overflow(), "expected overflow on deep nesting");
    }

    {
        json_writer w;
        w.begin_object();
        w.key("raw");
        w.raw("{\"k\":1}", 7);
        w.end_object();
        check_str(t, "json.raw-passthrough", w.data(), "{\"raw\":{\"k\":1}}");
    }

    {
        json_writer w;
        w.begin_object();
        w.key("b");
        w.boolean(true);
        w.key("n");
        w.null_value();
        w.end_object();
        check_str(t, "json.bool-null", w.data(), "{\"b\":true,\"n\":null}");
    }
}

/* ------------------------------ base64 ------------------------------ */
void test_base64(test_ctx *t)
{
    {
        char out[32];
        size_t n = base64_encode("hello", 5, out, sizeof out);
        check_bool(t, "base64.encode-vector", n == 8 && strcmp(out, "aGVsbG8=") == 0, "hello vector");
    }

    {
        char   out[64];
        char   back[64];
        size_t n = base64_encode("hello", 5, out, sizeof out);
        size_t m = base64_decode(out, n, back, sizeof back);
        check_bool(t, "base64.roundtrip", m == 5 && memcmp(back, "hello", 5) == 0, "roundtrip failed");
    }

    {
        char out[128];
        char back[128];
        bool ok = true;
        for (size_t len = 0; len <= 9; len++)
        {
            unsigned char src[16];
            for (size_t i = 0; i < len; i++)
                src[i] = (unsigned char)(0xA0 + i);
            size_t en = base64_encode(src, len, out, sizeof out);
            size_t de = base64_decode(out, en, back, sizeof back);
            if (en == (size_t)-1 || de != len || (len > 0 && memcmp(back, src, len) != 0))
            {
                ok = false;
                break;
            }
        }
        check_bool(t, "base64.roundtrip-all-lengths", ok, "length sweep failed");
    }

    {
        /* 拒绝：1 个字符（截断）、4 个填充、非法字符。
         * ❗**无填充**（`n % 4 == 3`）是合法写法 → `aGVsbG8` 必须解出 `hello`（不报错）。 */
        char   back[16];
        char   unpadded[16];
        size_t a  = base64_decode("a", 1, back, sizeof back);
        size_t b  = base64_decode("====", 4, back, sizeof back);
        size_t c  = base64_decode("ab$d", 4, back, sizeof back);
        size_t u  = base64_decode("aGVsbG8", 7, unpadded, sizeof unpadded);
        check_bool(t, "base64.reject-invalid", a == (size_t)-1 && b == (size_t)-1 && c == (size_t)-1,
                   "expected rejection");
        check_bool(t, "base64.accept-unpadded",
                   u == 5 && memcmp(unpadded, "hello", 5) == 0, "unpadded 'aGVsbG8' must decode to 'hello'");
    }
}

/* ------------------------------ rand ------------------------------ */
void test_rand(test_ctx *t){
    char a[33];
    char b[33];
    bool oka = rand_hex(a, sizeof a, 16);
    bool okb = rand_hex(b, sizeof b, 16);
    check_bool(t, "rand.hex-shape", oka && okb && strlen(a) == 32 && strlen(b) == 32, "expected 32 hex chars");
    check_bool(t, "rand.hex-changes", strcmp(a, b) != 0, "two draws were identical");
    check_bool(t, "rand.hex-rejects-small-cap", !rand_hex(a, 8, 16), "cap check");
}

/* ------------------------------ 帧头编解码（T2 / plan_cli.md §2.1） ------------------------------
 * 期望值全是手写字面量：帧头是小端定长 8 字节（u8 type | u8 flags | u16 id | u32 data_len），
 * 只要字节序或字段宽度变了，这里的字面量就会对不上——这正是要钉死的东西。 */
void test_frame(test_ctx *t)
{
    {
        frame_header h;
        h.type     = 2;
        h.flags    = 1;
        h.id       = 0x1234;
        h.data_len = 0x00020304;   /* 必须 ≤ k_frame_max_payload（8 MB）：0x08000000 以上会被拒 */

        unsigned char buf[k_frame_header_size];
        bool          ok = frame_header_encode(buf, sizeof buf, h);
        const char    want[k_frame_header_size] = {(char)0x02, (char)0x01, (char)0x34, (char)0x12,
                                                   (char)0x04, (char)0x03, (char)0x02, (char)0x00};
        check_bool(t, "frame.hdr.encode", ok, "expected success");
        check_bool(t, "frame.hdr.encode-little-endian", memcmp(buf, want, sizeof want) == 0, "u8|u8|u16|u32 LE");

        /* 拒绝路径：缓冲不够 / type 越界 / id 越界 */
        check_bool(t, "frame.hdr.encode-short-cap", !frame_header_encode(buf, 7, h), "cap < 8");
        frame_header bad = h;
        bad.type         = 0;
        check_bool(t, "frame.hdr.encode-type-zero", !frame_header_encode(buf, sizeof buf, bad), "type == 0");
        bad      = h;
        bad.id   = 0x10000u;
        check_bool(t, "frame.hdr.encode-id-overflow", !frame_header_encode(buf, sizeof buf, bad), "id > 0xFFFF");
        bad           = h;
        bad.data_len  = k_frame_max_payload + 1u;
        check_bool(t, "frame.hdr.encode-data-over-max", !frame_header_encode(buf, sizeof buf, bad), "data_len > max");

        /* 往返 */
        frame_header got;
        bool         dok = frame_header_decode(buf, sizeof buf, &got);

        /* ❗注意：上面的 `buf` 可能被拒绝路径写成半成品，往返用重新编一次的结果 */
        frame_header_encode(buf, sizeof buf, h);
        dok = frame_header_decode(buf, sizeof buf, &got);
        check_bool(t, "frame.hdr.decode-roundtrip",
                   dok && got.type == 2 && got.flags == 1 && got.id == 0x1234 && got.data_len == 0x00020304, "roundtrip");

        /* 拒绝路径：长度不够 / type 为 0 / data_len 超上限 */
        check_bool(t, "frame.hdr.decode-short", !frame_header_decode(buf, 7, &got), "len < 8");
        unsigned char zero_type[k_frame_header_size] = {0, 0, 0, 0, 0, 0, 0, 0};
        check_bool(t, "frame.hdr.decode-type-zero", !frame_header_decode(zero_type, sizeof zero_type, &got), "type == 0");
        unsigned char over[k_frame_header_size] = {6, 0, 0, 0, (char)0x01, (char)0x00, (char)0x00, (char)0x08};
        check_bool(t, "frame.hdr.decode-data-over-max", !frame_header_decode(over, sizeof over, &got), "0x08000001 > max");
    }

    /* json 长度反推：payload 总长 = 8 + json + data */
    {
        size_t json_len = 0;
        check_bool(t, "frame.json-len-basic", frame_json_len(8 + 3 + 5, 5, &json_len) && json_len == 3, "json = 3");
        check_bool(t, "frame.json-len-zero-data", frame_json_len(8 + 7, 0, &json_len) && json_len == 7, "json = 7");
        check_bool(t, "frame.json-len-too-short", !frame_json_len(8 + 4, 5, &json_len), "payload < 8 + data_len");
        check_bool(t, "frame.json-len-over-max", !frame_json_len(k_frame_max_payload + 1u, 0, &json_len), "payload > max");
    }

    /* raw 传输层的长度前缀（u32 小端 = payload 总长） */
    {
        unsigned char pfx[4];
        size_t        back = 0;
        bool          eok  = frame_prefix_encode(pfx, sizeof pfx, 0x00001234u);
        const char    want[4] = {(char)0x34, (char)0x12, (char)0x00, (char)0x00};   /* u32 LE */
        check_bool(t, "frame.prefix.encode", eok && memcmp(pfx, want, sizeof want) == 0, "u32 LE");
        check_bool(t, "frame.prefix.roundtrip", frame_prefix_decode(pfx, sizeof pfx, &back) && back == 0x00001234u, "roundtrip");
        check_bool(t, "frame.prefix.encode-short-cap", !frame_prefix_encode(pfx, 3, 1), "cap < 4");
        check_bool(t, "frame.prefix.encode-over-max", !frame_prefix_encode(pfx, sizeof pfx, (size_t)k_frame_max_payload + 1u), "payload > max");
        unsigned char over[4] = {(char)0x01, (char)0x00, (char)0x00, (char)0x08};   /* 0x08000001 */
        check_bool(t, "frame.prefix.decode-over-max", !frame_prefix_decode(over, sizeof over, &back), "prefix > max");
        check_bool(t, "frame.prefix.decode-short", !frame_prefix_decode(pfx, 3, &back), "len < 4");
    }
}

/* ------------------------------ PNG / BMP（截图编码，S4） ------------------------------ */
void test_image(test_ctx *t)
{
    /* 2x2 图（BGRA 字节序）：左上红、右上绿、左下蓝、右下白 */
    unsigned char px[2 * 2 * 4] = {
        0, 0, 255, 0,     /* 红 */
        0, 255, 0, 0,     /* 绿 */
        255, 0, 0, 0,     /* 蓝 */
        255, 255, 255, 0, /* 白 */
    };

    {
        unsigned char *png = nullptr;
        size_t         n   = png_encode_bgra(px, 2, 2, 8, &png);
        bool ok = (png != nullptr) && n > 8 && png[0] == 0x89 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G';
        check_bool(t, "png.signature", ok, "PNG 签名不对");
        if (ok)
        {
            check_bool(t, "png.ihdr", memcmp(png + 12, "IHDR", 4) == 0, "IHDR 缺失");
            check_int(t, "png.width", (long long)((png[16] << 24) | (png[17] << 16) | (png[18] << 8) | png[19]), 2, nullptr);
            check_int(t, "png.height", (long long)((png[20] << 24) | (png[21] << 16) | (png[22] << 8) | png[23]), 2, nullptr);
            check_int(t, "png.bit-depth", (long long)png[24], 8, nullptr);
            check_int(t, "png.color-type", (long long)png[25], 6, nullptr); /* 6 = RGBA */
            check_bool(t, "png.iend-tail", n >= 12 && memcmp(png + n - 8, "IEND", 4) == 0, "IEND 缺失");

            bool has_idat = false;
            for (size_t i = 4; i + 4 < n; i++)
            {
                if (memcmp(png + i, "IDAT", 4) == 0)
                {
                    has_idat = true;
                    /* 原始扫描线 = 2 行 × (1 + 2×4) = 18 字节；zlib 流 >= 18 + 2(头) + 4(adler) */
                    unsigned chunk_len = ((unsigned)png[i - 4] << 24) | ((unsigned)png[i - 3] << 16) |
                                         ((unsigned)png[i - 2] << 8) | (unsigned)png[i - 1];
                    check_bool(t, "png.idat-size", chunk_len >= 18 + 6, "IDAT 太小");
                    break;
                }
            }
            check_bool(t, "png.idat-exists", has_idat, "IDAT 缺失");
        }
        if (png)
            release(png);
    }

    {
        unsigned char *bmp = nullptr;
        size_t         n   = bmp_encode_bgra(px, 2, 2, 8, &bmp);
        /* 2px 宽 × 3B = 6 → 行对齐到 8；数据 16；总 70 */
        bool ok = (bmp != nullptr) && n == 54 + 2 * 8 && bmp[0] == 'B' && bmp[1] == 'M';
        check_bool(t, "bmp.header", ok, "BMP 头或长度不对");
        if (ok)
        {
            check_int(t, "bmp.width", (long long)(bmp[18] | (bmp[19] << 8)), 2, nullptr);
            check_int(t, "bmp.height", (long long)(bmp[22] | (bmp[23] << 8)), 2, nullptr);
            check_int(t, "bmp.bpp", (long long)(bmp[28] | (bmp[29] << 8)), 24, nullptr);
        }
        if (bmp)
            release(bmp);
    }

    {
        unsigned char *png = nullptr;
        check_bool(t, "png.rejects-zero-width", png_encode_bgra(px, 0, 2, 8, &png) == 0, "0 宽应失败");
        check_bool(t, "png.rejects-short-stride", png_encode_bgra(px, 2, 2, 4, &png) == 0, "stride 不足应失败");
    }
}
} /* namespace */

int run_selftests(selftest_report_fn report, void *ctx)
{
    test_ctx t;
    t.report = report;
    t.ctx    = ctx;
    t.total  = 0;
    t.failed = 0;

    test_http_parse(&t);
    test_http_response(&t);
    test_json(&t);
    test_base64(&t);
    test_rand(&t);
    test_frame(&t);
    test_image(&t);

    {
        char line[96];
        /* 汇总行：`[done] total=N failed=M`（不依赖 CRT 格式化） */
        const char *pfx = "[done] total=";
        size_t      n   = 0;
        for (const char *p = pfx; *p != '\0'; p++)
            line[n++] = *p;
        char num[12];
        int  k = 0;
        unsigned v = t.total;
        do
        {
            num[k++] = (char)('0' + (v % 10));
            v /= 10;
        } while (v != 0);
        while (k > 0)
            line[n++] = num[--k];
        const char *mid = " failed=";
        for (const char *p = mid; *p != '\0'; p++)
            line[n++] = *p;
        k = 0;
        v = t.failed;
        do
        {
            num[k++] = (char)('0' + (v % 10));
            v /= 10;
        } while (v != 0);
        while (k > 0)
            line[n++] = num[--k];
        line[n] = '\0';
        if (report != nullptr)
            report(ctx, line);
    }

    return (int)t.failed;
}
} /* namespace autodbg::common */
