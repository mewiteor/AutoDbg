/* =============================================================================
 * src/common/namespace.cpp —— 目录骨架锚点（P1）
 *
 * 本目录 = `autodbg::common`：core 与 scripting 的**共享基础设施**。
 *   分配器（全局 operator new/delete）、轻量容器、dynapi（新 API 动态加载层）、
 *   按需的 String（见 Plan.md 4.7 / 2.*）。
 *
 * 约束：
 *   * 不含调试器逻辑，也**不得感知 Lua**（由 tests/tools/check_core_has_no_lua.ps1 断言）；
 *   * 不引入 `std::` 容器/算法（零 CRT + 体积纪律）。
 *
 * 本文件只是骨架，用来验证构建系统三件事：
 *   ① 新目录进了 includedirs；② `.cpp` 被规则识别并按扩展名用 `/std:c++20` 编译；
 *   ③ 不含 CRT/new 依赖的 C++ 对象能正常链入零 CRT 产物。
 *   P2 起会被真实实现（allocator.cpp / containers.hpp / dynapi.cpp …）替换。
 * ========================================================================== */

namespace autodbg::common
{
static_assert(__cplusplus >= 202002L, "需要 C++20（xmake.lua 按扩展名选择 /std:）");

/* 仅用于给目标文件一点实际内容（可被链接器丢弃，无所谓） */
const char *module_name()
{
    return "common";
}
}
