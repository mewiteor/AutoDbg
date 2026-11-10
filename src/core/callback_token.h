#ifndef AUTODBG_CORE_CALLBACK_TOKEN_H
#define AUTODBG_CORE_CALLBACK_TOKEN_H
/* =============================================================================
 * src/core/callback_token.h —— 断点回调令牌（**宿主语言无关**，Plan 5.5 / A6）
 *
 * 引擎只对令牌做三件事：存下、命中时按值还回去、判断「有没有」。
 * 它不知道令牌代表什么，也不依赖任何宿主语言的句柄取值约定。
 *
 * 设计要点（三次迭代后的结论）：
 *   ① 不用整数哨兵（如 LUA_NOREF/-1）：那会让 core 被迫知晓宿主数值约定；
 *   ② 不用「把句柄位模式塞进 void*」的整数 pun：那是在赌「句柄永不为 0」
 *      —— 一旦为 0 就与「无回调」撞车，**静默丢回调**；换成别的宿主也不成立；
 *   ③ 也不用「每个令牌分配一个 holder」：多一次 alloc/free，而这里的语义
 *      本来就是**值拷贝**（令牌极小、只读、随断点节点一起生灭）。
 *
 * 因此用「句柄（union，按值） + 显式有效位」——**零堆分配、零哨兵、零类型 pun**：
 *   · 宿主句柄是指针（Python/Node 的 handle 都是指针）→ 用 `.ptr`
 *   · 宿主句柄是整数（如 Lua 的 registry ref）→ 用 `.i`
 *   · 无效时 `valid == 0`，此时 union 的取值**无意义也永远不会被读**
 * 引擎侧只拷贝整个 `callback_ref_t`（trivially copyable），从不读取 union 成员。
 *
 * 若将来某宿主确实需要「大于指针宽度的按值令牌」，再考虑把 session 模板化到
 * `TokenT`（见 TODO 阶段 3）；届时 C ABI 门面仍可用 `void*` 特化。
 * ========================================================================== */
#include <stdint.h>

/* 宿主句柄（按值）：指针或整数句柄，二选一，由绑定层决定用哪个成员。
 * ❗ pack(1)：它会被嵌进 `#pragma pack(1)` 的结构体（如 BreakpointInfo），
 *    默认对齐会在 x64 上多出 7 字节填充（16 而非 9）→ 白白变大。 */
#pragma pack(push, 1)

/* 宿主句柄（按值）：指针或整数句柄，二选一，由绑定层决定用哪个成员 */
typedef union callback_token_t
{
    void    *ptr; /* 指针句柄（PyObject* / napi_value / …） */
    intptr_t i;   /* 整数句柄（Lua registry ref 等） */
} callback_token_t;

/* 「句柄 + 有没有」——引擎按值存进断点节点，并按值传给回调 */
typedef struct callback_ref_t
{
    callback_token_t token;
    unsigned char    valid; /* 0 = 无回调；非 0 = 有效。**不依赖 token 的取值** */
} callback_ref_t;

#pragma pack(pop)

#endif /* AUTODBG_CORE_CALLBACK_TOKEN_H */
