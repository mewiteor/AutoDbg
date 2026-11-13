/* =============================================================================
 * src/common/console_style.cpp —— `help` 上色的「判定」实现（T7.3 切片 B / D53）
 *
 * 来源：新写（`plan_t7.md` §11.3）。与旧实现的差异：无（切片 A 只有纯文本）。
 * 本文件只做两件事：「这个输出能不能上色」（纯函数，自测直接断言）与「用户是不是用
 *   `NO_COLOR` 关掉了」（环境探测）。设置属性与写句柄一律在 `core::print_styled()`，
 *   本层不碰任何输出句柄。
 * ========================================================================== */
#include "console_style.hpp"

namespace autodbg::common
{
bool console_style_enabled(bool is_console, bool no_color_env)
{
    return is_console && !no_color_env;
}

bool no_color_requested()
{
    /* 缓冲区故意给 1 字节且**不读**：`GetEnvironmentVariableA` 的返回值是「需要的长度
     * （含结尾 NUL）」，所以任何非空值都会得到 != 0 的结果（值再长也只是缓冲区不够），
     * 未设置时才返回 0。空串在 Windows 上等于删除这个变量，因此「!= 0」正好等于
     * no-color.org 的「存在且非空」口径。 */
    char  probe[1];
    DWORD n = GetEnvironmentVariableA("NO_COLOR", probe, (DWORD)sizeof probe);
    return n != 0;
}
} /* namespace autodbg::common */
