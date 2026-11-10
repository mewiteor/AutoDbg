#ifndef AUTODBG_CORE_CONTEXT_HPP
#define AUTODBG_CORE_CONTEXT_HPP
/* =============================================================================
 * src/core/context.hpp —— 线程上下文（P3.B 模块；取代 update_context_sub.h 的 X-macro）
 *
 * 设计要点（消重）：
 *   · **字段表**（`ctx_field`）一次描述所有 CONTEXT 字段：名字（宿主可见键名）、
 *     偏移（`offsetof`）、形态（kind）。读/写两个方向共用同一张表 →
 *     加一个寄存器 = 表里加一行，不再需要在两个方向各写一遍宏。
 *   · 表是**宿主无关的数据**（名字只是标签）→ 将来 Python/Node 绑定可复用同一张表。
 *   · 名字与顺序沿用旧 X-macro（含 `C` 前缀的 `Cax`…`Cip`、`_ZT` 的 R/E 分区），
 *     因为黄金数据依赖它们逐字不变。
 *   · 本文件**不包含任何 Lua**（绑定层负责把表映射成宿主表示）。
 *
 * 覆盖（与旧实现完全一致）：
 *   x86 生效 23 项；x64 生效 60 项（多出的 37 项为 R8-R15/Xmm0-15/P1Home../
 *   VectorControl/DebugControl/LastBranch* 等）。
 *   x86 专属的 `FloatSave`/`ExtendedRegisters` 旧实现**未暴露** → 这里同样不暴露。
 * ========================================================================== */
#include "core.hpp"   /* P6.4：引擎函数已是 DebuggerSession 成员（下面的过渡转发也要它） */
#include "types.hpp"

#include <stdint.h>
#include <stddef.h>

namespace autodbg::core
{
/* 字段形态：决定绑定层怎么读/写它。
 * ❗ 枚举名要避开 lua_variant.h 的旧宏：它同时定义了**小写** `uz`（→ u32/u64）
 *   和**大写** `UZ`（→ 字符串 "U32"/"U64"）→ 两个都不能用（踩过两次：
 *   小写导致枚举重定义，大写导致枚举体里出现字符串字面量）。这里用 `UPTR`。 */
enum class ctx_kind : uint8_t
{
    U32,  /* 4 字节 */
    U64,  /* 8 字节 */
    UPTR, /* 指针宽度（x86=4 / x64=8）；旧 X-macro 的 _Z / _ZT */
    M128, /* M128A：{Low, High} 两个 64 位（旧 _128） */
};

struct ctx_field
{
    const char *name;   /* 宿主可见键名（旧 Lua 键名逐字保留） */
    uint32_t    offset; /* offsetof(CONTEXT, 字段) */
    ctx_kind    kind;
};

/* 返回字段表（按当前架构选择）与条目数 */
const ctx_field *context_fields(size_t *count);

/* ------------------------------ 线程上下文薄封装 ------------------------------
 * 统一：ZeroMem + 设 ContextFlags + 调用 + 失败时 E(...)；
 * 旧代码在 get/set 两处各抄一遍前导，这里只有一处。 */

/* 对「已知线程表」（会话里的 `threads()`）里的每个线程做 取 → mutate → 写回；
 * mutate 返回 false 表示该线程不改写（旧代码是 Get 失败就 continue）。
 * 旧实现在 4 个硬件断点函数里各抄一遍这个循环 → 现在只有一处。 */
}



#endif /* AUTODBG_CORE_CONTEXT_HPP */
