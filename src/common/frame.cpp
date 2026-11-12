/* =============================================================================
 * src/common/frame.cpp —— 帧头编解码实现（见 frame.hpp 的布局说明）
 *
 * 来源：新写（T2）。原先这段打包逻辑内联在 `scripting/lua/stream.cpp` 的
 *   `stream_write_frame` 里，只有「写」没有「读」，也没法被 `--selftest` 覆盖。
 * 与旧实现的差异：抽出纯函数 + 补齐读方向 + 全套边界检查（旧代码假定调用方给的
 *   data_len 一定合法）。
 * ========================================================================== */
#include "frame.hpp"

namespace autodbg::common
{
void frame_put_u16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
}

void frame_put_u32(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
    p[2] = (unsigned char)((v >> 16) & 0xFFu);
    p[3] = (unsigned char)((v >> 24) & 0xFFu);
}

unsigned frame_get_u16(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

unsigned frame_get_u32(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

bool frame_header_encode(unsigned char *dst, size_t cap, const frame_header &h)
{
    if (dst == nullptr || cap < (size_t)k_frame_header_size)
        return false;
    if (h.type == 0 || h.type > 0xFFu || h.flags > 0xFFu || h.id > 0xFFFFu)
        return false;
    if (h.data_len > k_frame_max_payload)
        return false;

    dst[0] = (unsigned char)(h.type & 0xFFu);
    dst[1] = (unsigned char)(h.flags & 0xFFu);
    frame_put_u16(dst + 2, h.id);
    frame_put_u32(dst + 4, h.data_len);
    return true;
}

bool frame_header_decode(const unsigned char *src, size_t len, frame_header *out)
{
    if (src == nullptr || out == nullptr || len < (size_t)k_frame_header_size)
        return false;

    unsigned type     = (unsigned)src[0];
    unsigned flags    = (unsigned)src[1];
    unsigned id       = frame_get_u16(src + 2);
    unsigned data_len = frame_get_u32(src + 4);

    if (type == 0 || data_len > k_frame_max_payload)
        return false;

    out->type     = type;
    out->flags    = flags;
    out->id       = id;
    out->data_len = data_len;
    return true;
}

bool frame_json_len(size_t payload_len, unsigned data_len, size_t *json_len)
{
    if (json_len == nullptr)
        return false;
    size_t need = (size_t)k_frame_header_size + (size_t)data_len;
    if (data_len > k_frame_max_payload || payload_len < need || payload_len > k_frame_max_payload)
        return false;
    *json_len = payload_len - need;
    return true;
}

bool frame_prefix_encode(unsigned char *dst, size_t cap, size_t payload_len)
{
    if (dst == nullptr || cap < 4 || payload_len > k_frame_max_payload)
        return false;
    frame_put_u32(dst, (unsigned)payload_len);
    return true;
}

bool frame_prefix_decode(const unsigned char *src, size_t len, size_t *payload_len)
{
    if (src == nullptr || payload_len == nullptr || len < 4)
        return false;
    unsigned v = frame_get_u32(src);
    if (v > k_frame_max_payload)
        return false;
    *payload_len = (size_t)v;
    return true;
}
} /* namespace autodbg::common */
