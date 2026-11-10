#ifndef AUTODBG_COMMON_BASE64_HPP
#define AUTODBG_COMMON_BASE64_HPP
/* =============================================================================
 * src/common/base64.hpp —— Base64 编解码（零依赖）
 *
 * 来源：新写（plan_server.md S0）。
 * 用途：把二进制（截图 PNG、内存 dump）塞进 JSON 字符串与 HTTP 响应（§7.1 / §8.4）。
 * 约束：零 CRT 算法实现，只用 `stddef.h`；不分配内存（调用方给缓冲）。
 * ========================================================================== */
#include <stddef.h>

namespace autodbg::common
{
/* 编码后长度（不含结尾 NUL） */
size_t base64_encoded_size(size_t n);

/* 编码：`out_cap` 至少 base64_encoded_size(n) + 1。
 * 返回写入的字符数（不含结尾 NUL）；空间不足返回 (size_t)-1。 */
size_t base64_encode(const void *src, size_t n, char *out, size_t out_cap);

/* 解码：忽略空白与换行（`\r` `\n` ` ` `\t`）。
 * 返回解码后的字节数；非法输入或空间不足返回 (size_t)-1。 */
size_t base64_decode(const char *src, size_t n, void *out, size_t out_cap);
} /* namespace autodbg::common */

#endif /* AUTODBG_COMMON_BASE64_HPP */
