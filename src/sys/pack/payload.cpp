/* =============================================================================
 * src/sys/pack/payload.cpp —— 载荷解析 / 解压（格式见 AGENTS.md §11.2 / D14）
 *
 * 来源：新写；布局见 payload.hpp 与 AGENTS.md §11.2。
 * 与旧实现的差异见 payload.hpp 头注释。
 * T8 切片 a1（D63）：entry 在 raw_size 之后多一个 `u8 kind`（0 = module、1 = exec blob）；
 *   本文件只多两件——读它 + 按 D11 的口径把它纳入边界校验（只允许 0 / 1），和按 kind 找 exec
 *   条目的最小访问点 `find_exec()`。解压口径不变：blob 的字节也在同一条单流里，
 *   「解压长度 == 各 raw_size 之和」的自检照旧。
 *
 * ❗解码必须自己提供 ELzmaStatus：`LzmaDecode` 入口无条件写 `*status`
 *   （PITFALLS.md K5），传 nullptr 必崩。
 * ========================================================================== */
#include "payload.hpp"

#include <string.h>   /* memcmp（影子头） */

#include "../lzma/Alloc.h"      /* g_Alloc */
#include "../lzma/LzmaDec.h"

namespace autodbg::sys::pack
{
namespace
{
/* 小端读取（不做对齐假设：载荷来自 `.c` 字节数组，可能奇数偏移） */
inline unsigned rd_u16(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

inline unsigned rd_u32(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

constexpr unsigned k_max_entries = 32;
constexpr unsigned k_max_name    = 63;   /* 与 tools/pack 的 pack_entry::name[64] 对齐 */
constexpr unsigned k_props_size  = 5;    /* LZMA_PROPS_SIZE */
}

payload::payload()
    : data_(nullptr)
    , size_(0)
    , count_(0)
    , raw_total_(0)
    , props_(nullptr)
    , stream_(nullptr)
    , payload_size_(0)
{
}

void payload::close()
{
    data_         = nullptr;
    size_         = 0;
    count_        = 0;
    raw_total_    = 0;
    props_        = nullptr;
    stream_       = nullptr;
    payload_size_ = 0;
}

bool payload::open(const unsigned char *data, unsigned size, load_error *err)
{
    close();

    if (data == nullptr || size < 4)
    {
        AUTODBG_PACK_FAIL(err, stage::payload, "payload truncated", size);
        return false;
    }

    data_ = data;
    size_ = size;

    unsigned off   = 0;
    unsigned count = rd_u32(data + off);
    off += 4;

    if (count == 0 || count > k_max_entries)
    {
        AUTODBG_PACK_FAIL(err, stage::payload, "entry_count out of range", count);
        return false;
    }

    unsigned raw_total = 0;
    for (unsigned i = 0; i < count; i++)
    {
        if (off + 2 > size)
        {
            AUTODBG_PACK_FAIL(err, stage::payload, "name_len truncated", i);
            return false;
        }
        unsigned name_len = rd_u16(data + off);
        off += 2;

        if (name_len == 0 || name_len > k_max_name || off + name_len > size)
        {
            AUTODBG_PACK_FAIL(err, stage::payload, "name_len out of range", name_len);
            return false;
        }
        const char *name = (const char *)(data + off);
        off += name_len;

        if (off + 4 > size)
        {
            AUTODBG_PACK_FAIL(err, stage::payload, "raw_size truncated", i);
            return false;
        }
        unsigned raw_size = rd_u32(data + off);
        off += 4;

        if (raw_size == 0 || raw_total + raw_size < raw_total)
        {
            AUTODBG_PACK_FAIL(err, stage::payload, "raw_size invalid", raw_size);
            return false;
        }

        /* D63：D11 没有 magic / 版本号，所以 kind 只能靠结构边界校验 */
        if (off + 1 > size)
        {
            AUTODBG_PACK_FAIL(err, stage::payload, "kind truncated", i);
            return false;
        }
        unsigned char kind = data[off];
        off += 1;

        if (kind > k_entry_kind_exec)
        {
            AUTODBG_PACK_FAIL(err, stage::payload, "kind out of range", kind);
            return false;
        }

        entries_[i].name       = name;
        entries_[i].name_len   = name_len;
        entries_[i].raw_size   = raw_size;
        entries_[i].kind       = kind;
        entries_[i].raw_offset = raw_total;
        raw_total += raw_size;
    }

    if (off + k_props_size > size)
    {
        AUTODBG_PACK_FAIL(err, stage::payload, "props truncated", off);
        return false;
    }
    props_ = data + off;
    off += k_props_size;

    if (off >= size)
    {
        AUTODBG_PACK_FAIL(err, stage::payload, "payload empty", 0);
        return false;
    }
    stream_       = data + off;
    payload_size_ = size - off;
    count_        = count;
    raw_total_    = raw_total;
    return true;
}

const char *payload::name(unsigned i) const
{
    return (i < count_) ? entries_[i].name : nullptr;
}

unsigned payload::name_len(unsigned i) const
{
    return (i < count_) ? entries_[i].name_len : 0;
}

unsigned payload::raw_size(unsigned i) const
{
    return (i < count_) ? entries_[i].raw_size : 0;
}

unsigned payload::raw_offset(unsigned i) const
{
    return (i < count_) ? entries_[i].raw_offset : 0;
}

unsigned char payload::kind(unsigned i) const
{
    return (i < count_) ? entries_[i].kind : k_entry_kind_module;
}

int payload::find_exec() const
{
    for (unsigned i = 0; i < count_; i++)
        if (entries_[i].kind == k_entry_kind_exec)
            return (int)i;
    return -1;
}

int payload::find_n(const char *name, unsigned name_len) const
{
    if (name == nullptr || name_len == 0)
        return -1;

    for (unsigned i = 0; i < count_; i++)
    {
        if (entries_[i].name_len != name_len)
            continue;
        if (memcmp(entries_[i].name, name, name_len) == 0)
            return (int)i;
    }
    return -1;
}

int payload::find(const char *name) const
{
    if (name == nullptr)
        return -1;

    unsigned len = 0;
    while (name[len] != '\0')
        len++;
    return find_n(name, len);
}

bool payload::extract_all(unsigned char *out, unsigned out_size, load_error *err) const
{
    if (out == nullptr || out_size != raw_total_)
    {
        AUTODBG_PACK_FAIL(err, stage::payload, "output buffer size mismatch", out_size);
        return false;
    }

    size_t       out_len = out_size;
    size_t       in_len  = payload_size_;
    ELzmaStatus  status;   /* 必须给：LzmaDecode 入口无条件写 *status（PITFALLS.md K5） */

    SRes r = LzmaDecode(out, &out_len, stream_, &in_len, props_, k_props_size,
                        LZMA_FINISH_ANY, &status, &g_Alloc);

    /* 单流 + FINISH_ANY：输出长度必须正好等于 manifest 总和，否则载荷被截断/篡改 */
    if (r != SZ_OK || out_len != raw_total_)
    {
        AUTODBG_PACK_FAIL(err, stage::payload, "LzmaDecode failed", (unsigned)r);
        return false;
    }
    return true;
}
}
