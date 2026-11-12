#ifndef AUTODBG_COMMON_FRAME_HPP
#define AUTODBG_COMMON_FRAME_HPP
/* =============================================================================
 * src/common/frame.hpp —— 帧头编解码（纯函数：不认 socket、不认 Lua、不认协议语义）
 *
 * 来源：新写（plan_cli.md T2 / D41–D45 的帧协议；从 `scripting/lua/stream.cpp`
 *   里把「打包/解包」这半边抽出来）。
 *
 * 帧布局（小端，固定 **8 字节头** + 可选 json + 可选原始数据）：
 *   u8  type | u8 flags | u16 id | u32 data_len | [json] | [data]
 *
 * ❗为什么头里**没有** json 长度：payload 的**总长由传输层给**（HTTP chunked 的
 *   hex 头 / raw TCP 的 u32 前缀），所以
 *       json_len = payload_len - 8 - data_len
 *   这样 8 字节头一个字都不用改，现有客户端（`nvim/lua/autodbg/stream.lua` 的解析器）
 *   也不用跟着改。这是 plan_cli.md §2.1 那张表的落地口径。
 *
 * 为什么放在 `common` 而不是 `scripting/lua`：`--selftest` 必须能断言编解码
 *   （AGENTS.md §4 的门禁要求），而 `common` 不能反向依赖绑定层。
 *
 * 约束：零 CRT + 无 STL；全部边界检查，失败返回 false，绝不越界写。
 * ========================================================================== */
#include <stddef.h>

namespace autodbg::common
{
enum
{
    k_frame_header_size = 8,                          /* u8 + u8 + u16 + u32 */
    k_frame_max_payload = 8u * 1024u * 1024u,         /* 与 `--server-max-frame` 同口径 */
};

/* 解出来的帧头（字段语义是协议层的事，本层只看数值范围） */
struct frame_header
{
    unsigned type;     /* 1..255（0 保留为非法） */
    unsigned flags;    /* out 帧：0 = stdout / 1 = stderr；其它帧为 0 */
    unsigned id;       /* u16：句柄号（open/data/close/read_req/read_resp） */
    unsigned data_len; /* u32：[data] 的字节数（**不含** json） */
};

void     frame_put_u16(unsigned char *p, unsigned v);
void     frame_put_u32(unsigned char *p, unsigned v);
unsigned frame_get_u16(const unsigned char *p);
unsigned frame_get_u32(const unsigned char *p);

/* 写 8 字节头。`cap < 8` / `type == 0` / `type > 255` / `id > 0xFFFF` / `flags > 0xFF` → false */
bool frame_header_encode(unsigned char *dst, size_t cap, const frame_header &h);

/* 读 8 字节头。`len < 8` / `type == 0` / `data_len > k_frame_max_payload` → false（`*out` 不改） */
bool frame_header_decode(const unsigned char *src, size_t len, frame_header *out);

/* payload 总长 → json 段长度。要求 `k_frame_header_size + data_len <= payload_len
 * <= k_frame_max_payload`，否则 false */
bool frame_json_len(size_t payload_len, unsigned data_len, size_t *json_len);

/* raw 传输层的长度前缀（u32，小端）= payload 总长。上限同样按 k_frame_max_payload 卡 */
bool frame_prefix_encode(unsigned char *dst, size_t cap, size_t payload_len);
bool frame_prefix_decode(const unsigned char *src, size_t len, size_t *payload_len);
} /* namespace autodbg::common */

#endif /* AUTODBG_COMMON_FRAME_HPP */
