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
 * ========================================================================== */
#include "json_writer.hpp"

#include "allocator.hpp"
#include "dbl2str.h" /* Ryu：最短且可精确回读的 double → 字符串 */

#include <string.h> /* memcpy（影子头 → msvcrt.dll） */

namespace autodbg::common
{
namespace
{
constexpr size_t k_default_cap = 256;

inline bool is_control(unsigned char c)
{
    return c < 0x20;
}
} /* namespace */

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
