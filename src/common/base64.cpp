/* =============================================================================
 * src/common/base64.cpp —— Base64 编解码实现
 *
 * 来源：新写（plan_server.md S0）。
 * 与旧实现的差异：无（新文件）。
 * 口径：标准字母表（RFC 4648 §4，`+/` 与 `=` 填充）；解码时忽略空白，
 *   对非法字符/坏填充一律返回 (size_t)-1（不做宽松容错，避免静默解码出垃圾）。
 * ========================================================================== */
#include "base64.hpp"

namespace autodbg::common
{
namespace
{
constexpr char k_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* 字符 → 6 bit 值；非法返回 -1 */
inline int decode_char(unsigned char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

inline bool is_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}
} /* namespace */

size_t base64_encoded_size(size_t n)
{
    return ((n + 2) / 3) * 4;
}

size_t base64_encode(const void *src, size_t n, char *out, size_t out_cap)
{
    if (out == nullptr)
        return (size_t)-1;
    size_t need = base64_encoded_size(n);
    if (out_cap < need + 1)
        return (size_t)-1;

    const unsigned char *p = (const unsigned char *)src;
    size_t               i = 0;
    size_t               o = 0;

    while (i + 3 <= n)
    {
        unsigned v = ((unsigned)p[i] << 16) | ((unsigned)p[i + 1] << 8) | (unsigned)p[i + 2];
        out[o++]   = k_alphabet[(v >> 18) & 0x3F];
        out[o++]   = k_alphabet[(v >> 12) & 0x3F];
        out[o++]   = k_alphabet[(v >> 6) & 0x3F];
        out[o++]   = k_alphabet[v & 0x3F];
        i += 3;
    }

    size_t rest = n - i;
    if (rest == 1)
    {
        unsigned v = ((unsigned)p[i] << 16);
        out[o++]   = k_alphabet[(v >> 18) & 0x3F];
        out[o++]   = k_alphabet[(v >> 12) & 0x3F];
        out[o++]   = '=';
        out[o++]   = '=';
    }
    else if (rest == 2)
    {
        unsigned v = ((unsigned)p[i] << 16) | ((unsigned)p[i + 1] << 8);
        out[o++]   = k_alphabet[(v >> 18) & 0x3F];
        out[o++]   = k_alphabet[(v >> 12) & 0x3F];
        out[o++]   = k_alphabet[(v >> 6) & 0x3F];
        out[o++]   = '=';
    }

    out[o] = '\0';
    return o;
}

size_t base64_decode(const char *src, size_t n, void *out, size_t out_cap)
{
    if (src == nullptr || out == nullptr)
        return (size_t)-1;

    unsigned char *dst   = (unsigned char *)out;
    size_t         o     = 0;
    unsigned       acc   = 0;
    int            bits  = 0;
    size_t         pads  = 0;

    for (size_t i = 0; i < n; i++)
    {
        unsigned char c = (unsigned char)src[i];
        if (is_space(c))
            continue;
        if (c == '=')
        {
            pads++;
            continue;
        }
        if (pads != 0) /* 填充后又有有效字符 → 非法 */
            return (size_t)-1;
        int v = decode_char(c);
        if (v < 0)
            return (size_t)-1;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            if (o >= out_cap)
                return (size_t)-1;
            dst[o++] = (unsigned char)((acc >> bits) & 0xFF);
        }
    }

    if (bits >= 6) /* 剩 6 位 = 字符数 %4==1，必然是截断的 */
        return (size_t)-1;
    if (bits > 0 && (acc & ((1u << bits) - 1u)) != 0) /* 填充位必须为 0 */
        return (size_t)-1;
    if (pads > 2)
        return (size_t)-1;
    return o;
}
} /* namespace autodbg::common */
