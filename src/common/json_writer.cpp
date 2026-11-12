/* =============================================================================
 * src/common/json_writer.cpp —— 极简 JSON 输出实现（只写不解析）
 *
 * 来源：新写（plan_server.md S0）。
 * 与旧实现的差异：无（新文件）。
 * 口径：
 *   · 缓冲从 `common::alloc` 起（默认 256 字节），按 1.5× 增长，上限由调用方决定（默认无上限）；
 *   · 任何一次写入越界 → 置 `m_overflow` 并**放弃后续写入**（已写入部分保持合法前缀）；
 *   · 字符串转义按 RFC 8259：`"` / `\` / 控制字符（< 0x20，用 `\u00XX` 与短转义）；
 *     非 ASCII 字节**原样输出**（项目内一律 UTF-8，`jsonx` 也是这个口径）。
 * T1（D50）的全转义出口（`string_escaped` / `raw_escaped`）——新增，不改 `string()` 的口径：
 *   · 非 ASCII 一律 `\uXXXX` ⇒ 输出纯 ASCII（详见 `json_writer.hpp` 的文件头）；
 *   · 优先按 UTF-8 解码；非法 UTF-8 的高位字节串按 **CP936** 整段解（legacy 变体的执行字符集）；
 *   · ❗降级：CP936 解不出（函数取不到 / 分配失败）时逐字节 `\u00XX` —— JSON 仍然合法，
 *     但读回的是字节值而不是字符。转换用**既有静态导入**（`MultiByteToWideChar`，w95 白名单内），
 *     不走 dynapi：dynapi 不会减少导入，反而可能把已放行的导入变成新的。
 * ========================================================================== */
#include "json_writer.hpp"

#include "allocator.hpp"
#include "dbl2str.h" /* Ryu：最短且可精确回读的 double → 字符串 */

#include <string.h> /* memcpy（影子头 → msvcrt.dll） */
#include <windows.h> /* MultiByteToWideChar / WideCharToMultiByte（**既有**静态导入，w95 白名单内） */

namespace autodbg::common
{
namespace
{
constexpr size_t   k_default_cap = 256;
constexpr unsigned k_cp936       = 936;   /* 项目里 legacy 变体的执行字符集（/execution-charset:gbk） */
constexpr unsigned long k_undecoded = 0xFFFFFFFFu;

inline bool is_control(unsigned char c)
{
    return c < 0x20;
}

/* UTF-8 序列长度（按首字节）；不是合法起始字节返回 0 */
size_t utf8_seq_len(unsigned char c)
{
    if (c >= 0xC0 && c <= 0xDF) return 2;
    if (c >= 0xE0 && c <= 0xEF) return 3;
    if (c >= 0xF0 && c <= 0xF4) return 4;   /* 0xF5.. 超出 Unicode 范围 */
    return 0;
}

/* 解 `s[i]` 起的一个 UTF-8 序列；失败（含前导字节/超出 BMP 平面之上的合法范围）返回 k_undecoded */
unsigned long utf8_decode(const char *s, size_t n, size_t i, size_t *consumed)
{
    *consumed = 0;
    size_t len = utf8_seq_len((unsigned char)s[i]);
    if (len == 0 || i + len > n)
        return k_undecoded;

    unsigned long cp = (unsigned long)((unsigned char)s[i] & (0xFFu >> (len + 1)));
    for (size_t k = 1; k < len; k++)
    {
        unsigned char b = (unsigned char)s[i + k];
        if ((b & 0xC0u) != 0x80u)
            return k_undecoded;
        cp = (cp << 6) | (unsigned long)(b & 0x3Fu);
    }

    /* 前导字节（overlong）与代理区（UTF-8 不允许直接编）都算非法 */
    unsigned long lowest = (len == 2) ? 0x80u : (len == 3) ? 0x800u : 0x10000u;
    if (cp < lowest || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu))
        return k_undecoded;

    *consumed = len;
    return cp;
}
} /* namespace */

/* `\uXXXX`（码点 > 0xFFFF 时写代理对） */
void json_writer::put_codepoint(unsigned long cp) noexcept
{
    static const char k_hex[] = "0123456789abcdef";

    unsigned long unit = cp;
    if (cp > 0xFFFFu)
    {
        unsigned long x = cp - 0x10000u;
        unit = 0xD800u + (x >> 10);
    }

    char esc[6];
    esc[0] = '\\';
    esc[1] = 'u';
    esc[2] = k_hex[(unit >> 12) & 0xF];
    esc[3] = k_hex[(unit >> 8) & 0xF];
    esc[4] = k_hex[(unit >> 4) & 0xF];
    esc[5] = k_hex[unit & 0xF];
    put(esc, 6);

    if (cp <= 0xFFFFu)
        return;

    unsigned long x = cp - 0x10000u;
    unsigned long low = 0xDC00u + (x & 0x3FFu);
    char esc2[6];
    esc2[0] = '\\';
    esc2[1] = 'u';
    esc2[2] = k_hex[(low >> 12) & 0xF];
    esc2[3] = k_hex[(low >> 8) & 0xF];
    esc2[4] = k_hex[(low >> 4) & 0xF];
    esc2[5] = k_hex[low & 0xF];
    put(esc2, 6);
}

/* 一段**已知不是 UTF-8** 的高位字节串：按 CP936 整段解（GBK 字符可以是 1 或 2 字节）。
 * 成功返回 true（已把每个码点写成 `\uXXXX`）；取不到函数或解不出 → false（调用方逐字节）。 */
bool json_writer::put_escaped_cp936(const char *s, size_t n) noexcept
{
    if (n == 0)
        return false;

    wchar_t *w = (wchar_t *)alloc(n * sizeof(wchar_t));
    if (w == nullptr)
        return false;

    int wn = MultiByteToWideChar(k_cp936, 0, s, (int)n, w, (int)n);
    if (wn <= 0)
    {
        release(w);
        return false;
    }
    for (int i = 0; i < wn; i++)
        put_codepoint((unsigned long)w[i]);

    release(w);
    return true;
}

/* 一个非 ASCII 单元：优先按 UTF-8 解；非法则把从 i 起的极大高位字节串整段按 CP936 解；
 * 取不到函数或解不出就逐字节 `\u00XX`（降级）。返回消耗的字节数（>= 1）。 */
size_t json_writer::put_nonascii_unit(const char *s, size_t n, size_t i) noexcept
{
    static const char k_hex[] = "0123456789abcdef";

    size_t used = 0;
    unsigned long cp = utf8_decode(s, n, i, &used);
    if (cp != k_undecoded)
    {
        put_codepoint(cp);
        return used;
    }

    size_t run = i;
    while (run < n && (unsigned char)s[run] >= 0x80u)
        run++;
    if (put_escaped_cp936(s + i, run - i))
        return run - i;

    char esc[6];
    esc[0] = '\\';
    esc[1] = 'u';
    esc[2] = '0';
    esc[3] = '0';
    esc[4] = k_hex[((unsigned char)s[i] >> 4) & 0xF];
    esc[5] = k_hex[(unsigned char)s[i] & 0xF];
    put(esc, 6);
    return 1;
}

/* 全转义：控制字符照旧，非 ASCII 一律 `\uXXXX`（所以输出纯 ASCII） */
void json_writer::put_escaped_full(const char *s, size_t n) noexcept
{
    static const char k_hex[] = "0123456789abcdef";

    for (size_t i = 0; i < n; )
    {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80u)
        {
            switch (c)
            {
            case '"': put("\\\"", 2); break;
            case '\\': put("\\\\", 2); break;
            case '\b': put("\\b", 2); break;
            case '\f': put("\\f", 2); break;
            case '\n': put("\\n", 2); break;
            case '\r': put("\\r", 2); break;
            case '\t': put("\\t", 2); break;
            default:
                if (is_control(c))
                {
                    char esc[6];
                    esc[0] = '\\';
                    esc[1] = 'u';
                    esc[2] = '0';
                    esc[3] = '0';
                    esc[4] = k_hex[(c >> 4) & 0xF];
                    esc[5] = k_hex[c & 0xF];
                    put(esc, 6);
                }
                else
                {
                    put((char)c);
                }
                break;
            }
            i++;
            continue;
        }

        i += put_nonascii_unit(s, n, i);
    }
}

/* 只转非 ASCII（raw 片段用）：结构字符（`"` / `\` / 括号）原样保留，否则片段就不是 JSON。
 * jsonx 的输出总是合法 JSON，所以高位字节只可能出现在串内 —— 写成 `\uXXXX` 不改变值。 */
void json_writer::put_escaped_nonascii(const char *s, size_t n) noexcept
{
    size_t i = 0;
    while (i < n)
    {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80u)
        {
            put((char)c);
            i++;
            continue;
        }
        i += put_nonascii_unit(s, n, i);
    }
}

void json_writer::string_escaped(const char *s) noexcept
{
    if (s == nullptr)
    {
        null_value();
        return;
    }
    string_escaped(s, strlen(s));
}

void json_writer::string_escaped(const char *s, size_t n) noexcept
{
    if (s == nullptr)
    {
        null_value();
        return;
    }
    before_value();
    put('"');
    put_escaped_full(s, n);
    put('"');
}

void json_writer::raw_escaped(const char *json, size_t n) noexcept
{
    if (json == nullptr || n == 0)
    {
        null_value();
        return;
    }
    before_value();
    put_escaped_nonascii(json, n);
}

json_writer::json_writer() noexcept : json_writer(k_default_cap) {}

json_writer::json_writer(size_t cap) noexcept
{
    if (cap < 32)
        cap = 32;
    m_buf = (char *)alloc(cap);
    if (m_buf == nullptr)
    {
        m_overflow = true;
        return;
    }
    m_cap = cap;
    m_buf[0] = '\0';
    m_stack[0]      = container::root;
    m_need_comma[0] = false;
}

json_writer::~json_writer() noexcept
{
    if (m_buf != nullptr)
        release(m_buf);
}

void json_writer::reset() noexcept
{
    m_len      = 0;
    m_depth    = 0;
    m_overflow = (m_buf == nullptr);
    m_stack[0]      = container::root;
    m_need_comma[0] = false;
    if (m_buf != nullptr)
        m_buf[0] = '\0';
}

json_writer::container json_writer::top() const noexcept
{
    return m_stack[m_depth];
}

bool json_writer::reserve(size_t extra) noexcept
{
    if (m_overflow)
        return false;
    if (m_buf == nullptr)
    {
        m_overflow = true;
        return false;
    }
    if (m_len + extra + 1 <= m_cap)
        return true;

    size_t cap = m_cap;
    while (cap < m_len + extra + 1)
    {
        size_t next = cap + cap / 2;
        if (next <= cap) /* 溢出保护 */
        {
            m_overflow = true;
            return false;
        }
        cap = next;
    }
    char *p = (char *)resize(m_buf, cap);
    if (p == nullptr)
    {
        m_overflow = true;
        return false;
    }
    m_buf = p;
    m_cap = cap;
    return true;
}

void json_writer::put(char c) noexcept
{
    if (!reserve(1))
        return;
    m_buf[m_len++] = c;
    m_buf[m_len]   = '\0';
}

void json_writer::put(const char *s, size_t n) noexcept
{
    if (s == nullptr || n == 0)
        return;
    if (!reserve(n))
        return;
    memcpy(m_buf + m_len, s, n);
    m_len += n;
    m_buf[m_len] = '\0';
}

void json_writer::put_u64(unsigned long long v) noexcept
{
    char tmp[24];
    int  i = 0;
    do
    {
        tmp[i++] = (char)('0' + (int)(v % 10));
        v /= 10;
    } while (v != 0);
    while (i > 0)
        put(tmp[--i]);
}

void json_writer::put_escaped(const char *s, size_t n) noexcept
{
    static const char k_hex[] = "0123456789abcdef";

    for (size_t i = 0; i < n; i++)
    {
        unsigned char c = (unsigned char)s[i];
        switch (c)
        {
        case '"': put("\\\"", 2); break;
        case '\\': put("\\\\", 2); break;
        case '\b': put("\\b", 2); break;
        case '\f': put("\\f", 2); break;
        case '\n': put("\\n", 2); break;
        case '\r': put("\\r", 2); break;
        case '\t': put("\\t", 2); break;
        default:
            if (is_control(c))
            {
                char esc[6];
                esc[0] = '\\';
                esc[1] = 'u';
                esc[2] = '0';
                esc[3] = '0';
                esc[4] = k_hex[(c >> 4) & 0xF];
                esc[5] = k_hex[c & 0xF];
                put(esc, 6);
            }
            else
            {
                put((char)c);
            }
            break;
        }
    }
}

void json_writer::before_value() noexcept
{
    container c = top();
    if (c == container::root)
        return;
    if (m_need_comma[m_depth])
        put(',');
    m_need_comma[m_depth] = true;
}

void json_writer::begin_object() noexcept
{
    before_value();
    if (m_depth >= k_max_depth)
    {
        m_overflow = true;
        return;
    }
    put('{');
    m_depth++;
    m_stack[m_depth]      = container::object;
    m_need_comma[m_depth] = false;
    /* 空对象的逗号在 end_object 里判断（用 need_comma 无法区分「刚开」与「已写完」） */
}

void json_writer::end_object() noexcept
{
    if (m_depth == 0)
    {
        m_overflow = true;
        return;
    }
    m_depth--;
    put('}');
}

void json_writer::begin_array() noexcept
{
    before_value();
    if (m_depth >= k_max_depth)
    {
        m_overflow = true;
        return;
    }
    put('[');
    m_depth++;
    m_stack[m_depth]      = container::array;
    m_need_comma[m_depth] = false;
}

void json_writer::end_array() noexcept
{
    if (m_depth == 0)
    {
        m_overflow = true;
        return;
    }
    m_depth--;
    put(']');
}

void json_writer::key(const char *name) noexcept
{
    if (name == nullptr)
    {
        m_overflow = true;
        return;
    }
    size_t n = strlen(name);
    key(name, n);
}

void json_writer::key(const char *name, size_t n) noexcept
{
    if (top() != container::object)
    {
        m_overflow = true;
        return;
    }
    if (m_need_comma[m_depth])
        put(',');
    put('"');
    put_escaped(name, n);
    put('"');
    put(':');
    /* 冒号已写出：值前**不**再补逗号 → 这里把 need_comma 归零，
     * 由值的 before_value() 负责（它看到 false 就不补）。 */
    m_need_comma[m_depth] = false;
}

void json_writer::string(const char *s) noexcept
{
    if (s == nullptr)
    {
        null_value();
        return;
    }
    string(s, strlen(s));
}

void json_writer::string(const char *s, size_t n) noexcept
{
    if (s == nullptr)
    {
        null_value();
        return;
    }
    before_value();
    put('"');
    put_escaped(s, n);
    put('"');
}

void json_writer::number(long long v) noexcept
{
    before_value();
    if (v < 0)
    {
        put('-');
        put_u64((unsigned long long)(-(v + 1)) + 1ULL);
    }
    else
    {
        put_u64((unsigned long long)v);
    }
}

void json_writer::number(unsigned long long v) noexcept
{
    before_value();
    put_u64(v);
}

void json_writer::number(double v) noexcept
{
    before_value();

    /* ① JSON 没有 inf/nan 字面量 → 输出 null（语义由调用方判断）。
     *    判据：NaN 满足 `v != v`；Inf 满足 `v - v != 0`。不依赖 <float.h>。 */
    if (!(v == v) || !((v - v) == 0.0))
    {
        put("null", 4);
        return;
    }

    /* ② 用 Ryu（`dbl2str`）而不是 "%.6f"：它给出**最短且能精确回读**的十进制表示，
     *    即「字符串 → strtod → 二进制与原来完全一致」可被保证（selftest 有 round-trip 断言）；
     *    也不受 locale 影响（Ryu 直接写 '.'）。指数形式（如 1e+15）也是合法 JSON。 */
    char buf[48];
    dbl2str(buf, v);
    put(buf, strlen(buf));
}
void json_writer::boolean(bool v) noexcept
{
    before_value();
    if (v)
        put("true", 4);
    else
        put("false", 5);
}

void json_writer::null_value() noexcept
{
    before_value();
    put("null", 4);
}

void json_writer::raw(const char *json, size_t n) noexcept
{
    if (json == nullptr || n == 0)
    {
        null_value();
        return;
    }
    before_value();
    put(json, n);
}
} /* namespace autodbg::common */
