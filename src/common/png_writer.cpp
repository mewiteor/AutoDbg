/* =============================================================================
 * src/common/png_writer.cpp —— PNG（stored deflate）/ BMP 编码实现
 *
 * 来源：新写（plan_server.md S4 / D24）。
 * 与旧实现的差异：无（新文件）。
 * PNG 结构（全部手写，约 200 行）：
 *   签名(8) + IHDR(25) + IDAT(12+zlib) + IEND(12)
 *   zlib 流 = 0x78 0x01 + [stored block]* + adler32(4)
 *   每个 stored block = 1 字节头(BFINAL|BTYPE=00) + LEN(2) + NLEN(2) + 原始数据
 *   原始数据 = 每行的 [filter=0] + RGBA 像素
 * ❗CRC32 表与 adler32 都自己做（不用 zlib）；CRC 表放静态区惰性填充（不进文件体积）。
 * ========================================================================== */
#include "png_writer.hpp"

#include "allocator.hpp"

#include <string.h>

namespace autodbg::common
{
namespace
{
/* ------------------------------ CRC32 ------------------------------ */
unsigned      g_crc_table[256];
bool          g_crc_init = false;

void crc_init()
{
    if (g_crc_init)
        return;
    for (unsigned i = 0; i < 256; i++)
    {
        unsigned c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crc_table[i] = c;
    }
    g_crc_init = true;
}

unsigned crc_update(unsigned crc, const unsigned char *p, size_t n)
{
    crc_init();
    unsigned c = crc;
    for (size_t i = 0; i < n; i++)
        c = g_crc_table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c;
}

/* ------------------------------ adler32 ------------------------------ */
unsigned adler32_update(unsigned adler, const unsigned char *p, size_t n)
{
    unsigned a = adler & 0xFFFFu;
    unsigned b = (adler >> 16) & 0xFFFFu;
    for (size_t i = 0; i < n; i++)
    {
        a = (a + p[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

/* ------------------------------ 小写入器 ------------------------------ */
struct sink
{
    unsigned char *buf = nullptr;
    size_t         len = 0;
    size_t         cap = 0;
    bool           ok  = true;

    bool reserve(size_t extra)
    {
        if (!ok)
            return false;
        if (len + extra <= cap)
            return true;
        size_t c = (cap == 0) ? 1024 : cap;
        while (c < len + extra)
        {
            size_t next = c + c / 2;
            if (next <= c)
            {
                ok = false;
                return false;
            }
            c = next;
        }
        unsigned char *p = (unsigned char *)resize(buf, c);
        if (p == nullptr)
        {
            ok = false;
            return false;
        }
        buf = p;
        cap = c;
        return true;
    }

    void put(const void *data, size_t n)
    {
        if (!reserve(n))
            return;
        memcpy(buf + len, data, n);
        len += n;
    }

    void put_u8(unsigned v) { put(&v, 1); }

    void put_be32(unsigned v)
    {
        unsigned char b[4] = { (unsigned char)(v >> 24), (unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v };
        put(b, 4);
    }

    /* PNG chunk：长度(4) + 类型(4) + 数据 + CRC(4) */
    void chunk(const char *type, const void *data, size_t n)
    {
        put_be32((unsigned)n);
        size_t start = len;
        put(type, 4);
        if (n > 0)
            put(data, n);
        unsigned crc = crc_update(0xFFFFFFFFu, buf + start, 4 + n) ^ 0xFFFFFFFFu;
        put_be32(crc);
    }
};

/* 把 BGRA 行转成 RGBA（过滤字节 0 已在外部写好） */
void bgra_row_to_rgba(const unsigned char *src, unsigned char *dst, unsigned w)
{
    for (unsigned x = 0; x < w; x++)
    {
        dst[x * 4 + 0] = src[x * 4 + 2]; /* R */
        dst[x * 4 + 1] = src[x * 4 + 1]; /* G */
        dst[x * 4 + 2] = src[x * 4 + 0]; /* B */
        dst[x * 4 + 3] = 0xFF;           /* A（截图没有 alpha 语义，一律不透明） */
    }
}
} /* namespace */

size_t png_encode_bgra(const void *bgra, unsigned w, unsigned h, unsigned stride, unsigned char **out)
{
    if (out != nullptr)
        *out = nullptr;
    if (bgra == nullptr || w == 0 || h == 0 || out == nullptr)
        return 0;
    if (stride < w * 4)
        return 0;

    /* ---- 1) 原始扫描线（每行 1 字节过滤类型 + RGBA） ---- */
    size_t raw_len = (size_t)h * (1 + (size_t)w * 4);
    unsigned char *raw = (unsigned char *)alloc(raw_len);
    if (raw == nullptr)
        return 0;

    const unsigned char *src = (const unsigned char *)bgra;
    for (unsigned y = 0; y < h; y++)
    {
        unsigned char *row = raw + (size_t)y * (1 + (size_t)w * 4);
        row[0] = 0; /* filter: None */
        bgra_row_to_rgba(src + (size_t)y * stride, row + 1, w);
    }

    /* ---- 2) zlib：stored blocks ---- */
    sink z;
    z.put_u8(0x78);
    z.put_u8(0x01);
    size_t off = 0;
    while (off < raw_len && z.ok)
    {
        size_t chunk_len = raw_len - off;
        if (chunk_len > 65535)
            chunk_len = 65535;
        unsigned char head[5];
        bool     final = (off + chunk_len >= raw_len);
        head[0] = (unsigned char)(final ? 0x01 : 0x00);
        head[1] = (unsigned char)(chunk_len & 0xFF);
        head[2] = (unsigned char)((chunk_len >> 8) & 0xFF);
        head[3] = (unsigned char)(~chunk_len & 0xFF);
        head[4] = (unsigned char)((~chunk_len >> 8) & 0xFF);
        z.put(head, sizeof head);
        z.put(raw + off, chunk_len);
        off += chunk_len;
    }
    unsigned adler = adler32_update(1, raw, raw_len);
    z.put_be32(adler);
    release(raw);

    if (!z.ok)
    {
        release(z.buf);
        return 0;
    }

    /* ---- 3) PNG 容器 ---- */
    sink png;
    static const unsigned char k_sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    png.put(k_sig, sizeof k_sig);

    unsigned char ihdr[13];
    ihdr[0]  = (unsigned char)(w >> 24);
    ihdr[1]  = (unsigned char)(w >> 16);
    ihdr[2]  = (unsigned char)(w >> 8);
    ihdr[3]  = (unsigned char)w;
    ihdr[4]  = (unsigned char)(h >> 24);
    ihdr[5]  = (unsigned char)(h >> 16);
    ihdr[6]  = (unsigned char)(h >> 8);
    ihdr[7]  = (unsigned char)h;
    ihdr[8]  = 8;  /* bit depth */
    ihdr[9]  = 6;  /* color type: RGBA */
    ihdr[10] = 0;  /* compression */
    ihdr[11] = 0;  /* filter */
    ihdr[12] = 0;  /* interlace */
    png.chunk("IHDR", ihdr, sizeof ihdr);
    png.chunk("IDAT", z.buf, z.len);
    png.chunk("IEND", nullptr, 0);
    release(z.buf);

    if (!png.ok)
    {
        release(png.buf);
        return 0;
    }

    *out = png.buf;
    return png.len;
}

size_t bmp_encode_bgra(const void *bgra, unsigned w, unsigned h, unsigned stride, unsigned char **out)
{
    if (out != nullptr)
        *out = nullptr;
    if (bgra == nullptr || w == 0 || h == 0 || out == nullptr)
        return 0;
    if (stride < w * 4)
        return 0;

    unsigned row_size = (w * 3 + 3) & ~3u; /* 24bpp 行按 4 字节对齐 */
    size_t   data_len = (size_t)row_size * h;
    size_t   total    = 54 + data_len;

    unsigned char *buf = (unsigned char *)alloc(total);
    if (buf == nullptr)
        return 0;
    memset(buf, 0, 54);

    unsigned char *p = buf;
    p[0] = 'B';
    p[1] = 'M';
    /* 文件大小（54 + data） */
    p[2] = (unsigned char)total;
    p[3] = (unsigned char)(total >> 8);
    p[4] = (unsigned char)(total >> 16);
    p[5] = (unsigned char)(total >> 24);
    unsigned data_off = 54;
    p[10] = (unsigned char)data_off;
    unsigned dib = 40;
    p[14] = (unsigned char)dib;
    /* 宽/高（高为正 = bottom-up） */
    p[18] = (unsigned char)w;
    p[19] = (unsigned char)(w >> 8);
    p[20] = (unsigned char)(w >> 16);
    p[21] = (unsigned char)(w >> 24);
    p[22] = (unsigned char)h;
    p[23] = (unsigned char)(h >> 8);
    p[24] = (unsigned char)(h >> 16);
    p[25] = (unsigned char)(h >> 24);
    p[26] = 1;  /* planes */
    p[28] = 24; /* bpp */
    p[34] = (unsigned char)data_len;
    p[35] = (unsigned char)(data_len >> 8);
    p[36] = (unsigned char)(data_len >> 16);
    p[37] = (unsigned char)(data_len >> 24);

    const unsigned char *src = (const unsigned char *)bgra;
    for (unsigned y = 0; y < h; y++)
    {
        /* BMP 是 bottom-up：源第 y 行 → 目标第 (h-1-y) 行 */
        unsigned char       *dst = buf + data_off + (size_t)(h - 1 - y) * row_size;
        const unsigned char *s   = src + (size_t)y * stride;
        for (unsigned x = 0; x < w; x++)
        {
            dst[x * 3 + 0] = s[x * 4 + 0]; /* B */
            dst[x * 3 + 1] = s[x * 4 + 1]; /* G */
            dst[x * 3 + 2] = s[x * 4 + 2]; /* R */
        }
    }

    *out = buf;
    return total;
}
} /* namespace autodbg::common */
