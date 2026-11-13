/* =============================================================================
 * src/common/handoff_decision.cpp —— handoff 判定的纯函数实现（T8 切片 a1 / D63）
 *
 * 来源：新写（口径见 `handoff_decision.hpp` 的文件头）。本文件只有四个早退 + 一个 1，
 *   没有 I/O、没有分配、没有 Lua、没有系统调用；调用方（启动器）负责把四个 bool 探好后传进来。
 * 与旧实现的差异：无（新文件）。
 * 门禁：`src/common/selftest.cpp` 的 `handoff.decision-*` 四条（73 → 77）。
 * ========================================================================== */
#include "handoff_decision.hpp"

namespace autodbg::common
{
int decide_handoff_action(bool host_is_64, bool has_exec_entry, bool main_exe_only_option, bool explicit_legacy)
{
    /* ① 归主 exe 的选项（-h / --list / --version / --selftest） */
    if (main_exe_only_option)
        return 0;

    /* ② 载荷里没有 exec 条目 */
    if (!has_exec_entry)
        return 0;

    /* ③ host 不是 64 位（含 IsWow64Process 取不到的那一档） */
    if (!host_is_64)
        return 0;

    /* ④ 显式 --variant=legacy：排障开关优先 */
    if (explicit_legacy)
        return 0;

    return 1;
}
} /* namespace autodbg::common */
