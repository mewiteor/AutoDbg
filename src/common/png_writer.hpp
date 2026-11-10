#ifndef AUTODBG_COMMON_PNG_WRITER_HPP
#define AUTODBG_COMMON_PNG_WRITER_HPP
/* =============================================================================
 * src/common/png_writer.hpp —— PNG / BMP 编码（截图用，零依赖）
 *
 * 来源：新写（plan_server.md S4 / §7.3，决策 D24）。
 * 为什么自己写 PNG：截图要能直接进 JSON（MCP 的 image content）与 nvim，
 *   而 PNG 是唯一「任何客户端都认、又不用引第三方库」的选择；真压缩要 deflate，
 *   所以这里用 **stored（无压缩）deflate**：合法 zlib 流、任何解码器都能读，
 *   代价只是体积≈原始像素（可用 scale/裁剪把像素量降下来）。
 * 输入布局（与我们截图的 DIB 一致）：**BGRA、top-down、stride = w*4**。
 * 输出：新分配的缓冲（用 `common::release` 释放），返回字节数；失败返回 0。
 * 约束：零 CRT（只用 alloc/resize/release 与 memcpy）。
 * ========================================================================== */
#include <stddef.h>

namespace autodbg::common
{
/* PNG：8-bit RGBA（把输入 BGRA 的 R/B 交换），stored deflate */
size_t png_encode_bgra(const void *bgra, unsigned w, unsigned h, unsigned stride, unsigned char **out);

/* BMP：24bpp 未压缩、bottom-up（Windows 原生顺序），调试/兼容用 */
size_t bmp_encode_bgra(const void *bgra, unsigned w, unsigned h, unsigned stride, unsigned char **out);
} /* namespace autodbg::common */

#endif /* AUTODBG_COMMON_PNG_WRITER_HPP */
