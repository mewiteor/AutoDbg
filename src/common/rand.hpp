#ifndef AUTODBG_COMMON_RAND_HPP
#define AUTODBG_COMMON_RAND_HPP
/* =============================================================================
 * src/common/rand.hpp —— 随机字节（server token 用）
 *
 * 来源：新写（plan_server.md S0 / §4.7）。
 * 用途：`--server-token=auto` 生成 bearer token。
 * 熵源（实测可用性见 plan_server.md §1.4）：
 *   ① `advapi32!SystemFunction036`（RtlGenRandom）：XP+ 有，走 dynapi；
 *   ② 取不到时（Win95/98）降级为「时间 + 计数器 + PID/TID + 静态计数器」混合 —
 *      **不是密码学安全**，只够「避免随手猜到的固定值」；文档里已记录（§4.7）。
 * 约束：零 CRT（无 rand/srand），失败**不返回错误**（getrandom 语义：尽力填充）。
 * ========================================================================== */
#include <stddef.h>
#include <stdbool.h>

namespace autodbg::common
{
/* 用随机字节填充 dst（永不失败；取不到系统熵源时用混合降级） */
void rand_bytes(void *dst, size_t n) noexcept;

/* 生成 n_bytes 字节随机数的**小写 hex** 串（长度 2*n_bytes）+ NUL。
 * `cap` 不足（需要 2*n_bytes + 1）返回 false 且 dst 置空串。 */
bool rand_hex(char *dst, size_t cap, size_t n_bytes) noexcept;
} /* namespace autodbg::common */

#endif /* AUTODBG_COMMON_RAND_HPP */
