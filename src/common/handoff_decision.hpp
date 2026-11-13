/* =============================================================================
 * src/common/handoff_decision.hpp —— handoff（把执行交给落盘的 exec 条目）的「判定」（纯函数，T8 切片 a1 / D63）
 *
 * 职责：只回答「这次要不要 handoff」，返回值 1 = handoff、0 = 不 handoff（主 exe 自己跑 x86 模块）。
 * 来源：新写（plan_bootloader.md §2 / §5.5 的 (a)）。
 * 与旧实现的差异：无（新文件）。真正的 I/O（tmpdir 落盘、CreateProcess、job、透传退出码）在
 *   `src/sys/pack/handoff.cpp`（切片 a1 的下一步）；判定单独拆出来是因为它可单测。
 *
 * 为什么在 `common/`：这半截是**纯判定**（不做 I/O、不碰文件系统、不碰 Lua），输入全部由调用方
 *   探测后传进来；与 `console_action.*` 是同一条理由（`common` 不得依赖 `scripting/lua`，
 *   而启动器与模块两侧要用同一份口径）。签名只用 `bool`，所以连 `<windows.h>` 都不需要——
 *   `host_is_64` 来自调用方的 `IsWow64Process`（只有 wxp 有，走 dynapi；取不到 = 不是 64）。
 *
 * 已定规则（各自给出 0，其余才是 1）：
 *   ① `-h` / `--list` / `--version` / `--selftest` 归主 exe → 0（`--version` 因此不触发落盘，
 *      这是它能在只读目录上跑的原因，plan_bootloader.md §6 的 `version.no-extract`）；
 *   ② 没有 `exec` 条目 → 0（没有可执行的半份，只能映射 x86 模块；单架构 `make.bat a 86` 就是这种）；
 *   ③ host 不是 64 位 → 0（`IsWow64Process` 取不到就算不是 64，95/98 本来没有 64 位）；
 *   ④ 显式 `--variant=legacy` → 0（排障开关优先）：落盘的 x64 半份恒为 modern（D10 / D63），
 *      x64 host 上唯一能跑到 legacy 的路径就是主 exe（x86）自己跑。
 * ========================================================================== */
#ifndef AUTODBG_COMMON_HANDOFF_DECISION_HPP
#define AUTODBG_COMMON_HANDOFF_DECISION_HPP

namespace autodbg::common
{
/* 1 = handoff（跑落盘的 exec 条目）；0 = 不 handoff（主 exe 自己跑）。参数按已定规则列出。 */
int decide_handoff_action(bool host_is_64, bool has_exec_entry, bool main_exe_only_option, bool explicit_legacy);
} /* namespace autodbg::common */

#endif /* AUTODBG_COMMON_HANDOFF_DECISION_HPP */
