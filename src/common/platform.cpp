/* =============================================================================
 * src/common/platform.cpp —— 运行期平台事实（T7.3 切片 A / D51·D53）
 *
 * 来源：`launcher.cpp` 的 `detect_variant()` 判据 + `registry.cpp` 的 `_autodbg_info` 里的
 *   编译期字符串。搬到这里是因为**模块侧**也要用（`dbg.caps()` / `help` / 报错文案），
 *   而模块拿不到启动器的代码，也不能靠编译开关说「当前是 9x」。
 *
 * 与旧实现的差异：
 *   · `is_win9x()` 是运行期判定（旧代码只有启动器里的 `detect_variant()`）；
 *   · `arch_name` / `variant_name` / `flavor_name` 收拢成一处，免得 `caps` / `help` / `info`
 *     各写一份字符串（漏改一处就会互相矛盾）。
 * ========================================================================== */
#include "platform.hpp"

#include <windows.h>

namespace autodbg::common
{
bool is_win9x() noexcept
{
    /* 零初始化用聚合初始化（不 include core/util.hpp 的 ZeroMem）：common 不应该反向依赖 core。 */
    OSVERSIONINFOA vi{};
    vi.dwOSVersionInfoSize = sizeof vi;

    /* `GetVersionExA` 在新 SDK 里标了已废弃（仅因 manifest 机制，见 launcher.cpp 的注释）。
     * 这里确实需要它：`RtlGetVersion` 在 9x 上不存在，而我们要判的正是 9x。 */
#pragma warning(push)
#pragma warning(disable : 4996)
    if (GetVersionExA(&vi))
        return vi.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS; /* Win95/98/ME */
#pragma warning(pop)
    return false; /* 取不到就按「不是 9x」：宁可多报能力，也别把功能藏起来 */
}

bool is_nt() noexcept { return !is_win9x(); }

const char *arch_name() noexcept
{
#ifdef _WIN64
    return "x64";
#else
    return "x86";
#endif
}

const char *variant_name() noexcept
{
#ifdef NO_SCYLLA_HIDE
    return "legacy";
#else
    return "modern";
#endif
}

const char *flavor_name() noexcept
{
#ifdef JIT
    return "luajit";
#else
    return "lua";
#endif
}
} /* namespace autodbg::common */
