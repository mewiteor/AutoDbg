/* =============================================================================
 * src/common/platform.hpp —— 运行期平台事实（T7.3 切片 A / D51·D53）
 *
 * 来源：新写。把 `src/sys/pack/launcher.cpp` 里 `detect_variant()` 的判据抽出来，
 *   给**模块侧**用（启动器与模块都要判「是不是 Win95/98」，但启动器在 exe 里，
 *   模块拿不到它，也不能靠编译开关代劳）。
 *
 * 为什么不能靠编译开关：`/DAUTODBG_WIN9X`（legacy 变体）是「这套代码要能在 9x 上跑」的
 *   构建约束，不是「当前正在 9x 上跑」的事实 —— legacy 产物在 Win11 上照样运行。
 *   而 `dbg.caps()` 要报的是**这台机器**的能力（例如 `attach` 在 9x 上不可用），
 *   所以必须运行期判。
 *
 * 判据与启动器一致：`GetVersionExA` 的 `dwPlatformId == VER_PLATFORM_WIN32_WINDOWS`。
 *   新版 SDK 的 downlevel 谎言只影响 `dwMajorVersion`，不影响 `dwPlatformId`，
 *   所以「是不是 9x」这一条判定仍然可靠（见 launcher.cpp 的注释）。
 * ========================================================================== */
#ifndef AUTODBG_COMMON_PLATFORM_HPP
#define AUTODBG_COMMON_PLATFORM_HPP

namespace autodbg::common
{
/* Win95 / 98 / ME（VER_PLATFORM_WIN32_WINDOWS）。取不到版本信息时按「不是 9x」处理
 * （宁可在新系统上多报能力，也不要在新系统上把功能藏起来）。 */
bool is_win9x() noexcept;

/* `!is_win9x()` 的语义化别名：`DebugActiveProcess(Stop)`、`--detach`、`SetThreadContext`
 * 这些「NT 才有」的能力都用它做能力上报。 */
bool is_nt() noexcept;

/* 调试器自身位数（与目标无关）："x86" / "x64"。给 caps / help / 报错文案用。 */
const char *arch_name() noexcept;

/* 构建变体："modern" / "legacy"（由 `NO_SCYLLA_HIDE` 决定，与平台无关）。 */
const char *variant_name() noexcept;

/* 脚本 flavor："lua" / "luajit"（由 `JIT` 决定）。 */
const char *flavor_name() noexcept;
} /* namespace autodbg::common */

#endif /* AUTODBG_COMMON_PLATFORM_HPP */
