#ifndef AUTODBG_COMMON_SELFTEST_HPP
#define AUTODBG_COMMON_SELFTEST_HPP
/* =============================================================================
 * src/common/selftest.hpp —— 纯函数自测（server 相关基础设施的回归门禁）
 *
 * 来源：新写（plan_server.md S0 验收）。
 * 为什么放在产品模块里：本副本没有 `tests/` 目录，而 S1 起的传输层必须先在
 *   「解析/序列化」这一层有**可重复的断言**。放在 `common` 里既能被模块编译进去
 *   （`AutoDbg_<arch>.exe --selftest`），也能在 Win98 实机上顺手跑一遍（无外部依赖）。
 * 代价：约 3–5 KB 代码进模块（§9 风险表已登记）。
 * 约定：只测**纯函数**（http 解析/响应、json 输出、base64、rand 性质），
 *   不碰 socket、Lua、引擎——那些属于 S1+ 的 e2e（`tools/server_test/`）。
 * ========================================================================== */
#include <stddef.h>

namespace autodbg::common
{
/* 每行报告回调（形如 `[ok] http.parse.get-minimal` / `[FAIL] …: detail`） */
typedef void (*selftest_report_fn)(void *ctx, const char *line);

/* 返回 0 = 全部通过；否则 = 失败用例数 */
int run_selftests(selftest_report_fn report, void *ctx);
} /* namespace autodbg::common */

#endif /* AUTODBG_COMMON_SELFTEST_HPP */
