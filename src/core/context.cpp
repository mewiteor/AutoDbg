/* =============================================================================
 * src/core/context.cpp —— 线程上下文：字段表 + 薄封装（P3.B 模块）
 *
 * 取代 `update_context_sub.h`（X-macro：字段表 1 份、行为宏 2 套、前导代码 2 处）。
 * 本文件的字段表由旧 X-macro **脚本化逐项转换**而来（保序保名），因此不会漏项：
 *   SET_FIELD_32  -> ctx_kind::U32      SET_FIELD_64 -> ctx_kind::U64
 *   SET_FIELD_Z   -> ctx_kind::UPTR       SET_FIELD_ZT -> uz（键名加 C 前缀；寄存器 R 或 E 前缀）
 *   SET_FIELD_128 -> ctx_kind::M128     #ifdef _WIN64 区 → 原样保留为 x64 专属区
 * ========================================================================== */
#include "context.hpp"
#include "core.hpp" /* P6.4：引擎函数是会话成员（本 TU 多数定义即成员，直接用自身状态） */
#include "util.hpp"

#include <assert.h>
#include <intrin.h>
#include <string.h>

/* 类型统一（删 legacy_types.h）：这些名字现在只有 types.hpp 里的 `autodbg::core::` 那套 */
using autodbg::core::ThreadInfo;
using autodbg::core::DebuggerSession; /* P6.4：引擎函数已是它的成员 */

namespace autodbg::core
{
/* ------------------------------ 字段表 ------------------------------ */
static const ctx_field k_context_fields[] = {
    { "ContextFlags", offsetof(CONTEXT, ContextFlags), ctx_kind::U32 },
    { "SegGs", offsetof(CONTEXT, SegGs), ctx_kind::U32 },
    { "SegFs", offsetof(CONTEXT, SegFs), ctx_kind::U32 },
    { "SegEs", offsetof(CONTEXT, SegEs), ctx_kind::U32 },
    { "SegDs", offsetof(CONTEXT, SegDs), ctx_kind::U32 },
    { "SegCs", offsetof(CONTEXT, SegCs), ctx_kind::U32 },
    { "SegSs", offsetof(CONTEXT, SegSs), ctx_kind::U32 },
    { "EFlags", offsetof(CONTEXT, EFlags), ctx_kind::U32 },
    { "Dr0", offsetof(CONTEXT, Dr0), ctx_kind::UPTR  },
    { "Dr1", offsetof(CONTEXT, Dr1), ctx_kind::UPTR  },
    { "Dr2", offsetof(CONTEXT, Dr2), ctx_kind::UPTR  },
    { "Dr3", offsetof(CONTEXT, Dr3), ctx_kind::UPTR  },
    { "Dr6", offsetof(CONTEXT, Dr6), ctx_kind::UPTR  },
    { "Dr7", offsetof(CONTEXT, Dr7), ctx_kind::UPTR  },
#ifdef _WIN64
    { "Cax", offsetof(CONTEXT, Rax), ctx_kind::UPTR },
    { "Cbx", offsetof(CONTEXT, Rbx), ctx_kind::UPTR },
    { "Ccx", offsetof(CONTEXT, Rcx), ctx_kind::UPTR },
    { "Cdx", offsetof(CONTEXT, Rdx), ctx_kind::UPTR },
    { "Csi", offsetof(CONTEXT, Rsi), ctx_kind::UPTR },
    { "Cdi", offsetof(CONTEXT, Rdi), ctx_kind::UPTR },
    { "Csp", offsetof(CONTEXT, Rsp), ctx_kind::UPTR },
    { "Cbp", offsetof(CONTEXT, Rbp), ctx_kind::UPTR },
    { "Cip", offsetof(CONTEXT, Rip), ctx_kind::UPTR },
#else
    { "Cax", offsetof(CONTEXT, Eax), ctx_kind::UPTR },
    { "Cbx", offsetof(CONTEXT, Ebx), ctx_kind::UPTR },
    { "Ccx", offsetof(CONTEXT, Ecx), ctx_kind::UPTR },
    { "Cdx", offsetof(CONTEXT, Edx), ctx_kind::UPTR },
    { "Csi", offsetof(CONTEXT, Esi), ctx_kind::UPTR },
    { "Cdi", offsetof(CONTEXT, Edi), ctx_kind::UPTR },
    { "Csp", offsetof(CONTEXT, Esp), ctx_kind::UPTR },
    { "Cbp", offsetof(CONTEXT, Ebp), ctx_kind::UPTR },
    { "Cip", offsetof(CONTEXT, Eip), ctx_kind::UPTR },
#endif
#ifdef _WIN64   /* x64 专属字段（37 项） */
    { "P1Home", offsetof(CONTEXT, P1Home), ctx_kind::U64 },
    { "P2Home", offsetof(CONTEXT, P2Home), ctx_kind::U64 },
    { "P3Home", offsetof(CONTEXT, P3Home), ctx_kind::U64 },
    { "P4Home", offsetof(CONTEXT, P4Home), ctx_kind::U64 },
    { "P5Home", offsetof(CONTEXT, P5Home), ctx_kind::U64 },
    { "P6Home", offsetof(CONTEXT, P6Home), ctx_kind::U64 },
    { "MxCsr", offsetof(CONTEXT, MxCsr), ctx_kind::U32 },
    { "R8", offsetof(CONTEXT, R8), ctx_kind::U64 },
    { "R9", offsetof(CONTEXT, R9), ctx_kind::U64 },
    { "R10", offsetof(CONTEXT, R10), ctx_kind::U64 },
    { "R11", offsetof(CONTEXT, R11), ctx_kind::U64 },
    { "R12", offsetof(CONTEXT, R12), ctx_kind::U64 },
    { "R13", offsetof(CONTEXT, R13), ctx_kind::U64 },
    { "R14", offsetof(CONTEXT, R14), ctx_kind::U64 },
    { "R15", offsetof(CONTEXT, R15), ctx_kind::U64 },
    { "Xmm0", offsetof(CONTEXT, Xmm0), ctx_kind::M128 },
    { "Xmm1", offsetof(CONTEXT, Xmm1), ctx_kind::M128 },
    { "Xmm2", offsetof(CONTEXT, Xmm2), ctx_kind::M128 },
    { "Xmm3", offsetof(CONTEXT, Xmm3), ctx_kind::M128 },
    { "Xmm4", offsetof(CONTEXT, Xmm4), ctx_kind::M128 },
    { "Xmm5", offsetof(CONTEXT, Xmm5), ctx_kind::M128 },
    { "Xmm6", offsetof(CONTEXT, Xmm6), ctx_kind::M128 },
    { "Xmm7", offsetof(CONTEXT, Xmm7), ctx_kind::M128 },
    { "Xmm8", offsetof(CONTEXT, Xmm8), ctx_kind::M128 },
    { "Xmm9", offsetof(CONTEXT, Xmm9), ctx_kind::M128 },
    { "Xmm10", offsetof(CONTEXT, Xmm10), ctx_kind::M128 },
    { "Xmm11", offsetof(CONTEXT, Xmm11), ctx_kind::M128 },
    { "Xmm12", offsetof(CONTEXT, Xmm12), ctx_kind::M128 },
    { "Xmm13", offsetof(CONTEXT, Xmm13), ctx_kind::M128 },
    { "Xmm14", offsetof(CONTEXT, Xmm14), ctx_kind::M128 },
    { "Xmm15", offsetof(CONTEXT, Xmm15), ctx_kind::M128 },
    { "VectorControl", offsetof(CONTEXT, VectorControl), ctx_kind::U64 },
    { "DebugControl", offsetof(CONTEXT, DebugControl), ctx_kind::U64 },
    { "LastBranchToRip", offsetof(CONTEXT, LastBranchToRip), ctx_kind::U64 },
    { "LastBranchFromRip", offsetof(CONTEXT, LastBranchFromRip), ctx_kind::U64 },
    { "LastExceptionToRip", offsetof(CONTEXT, LastExceptionToRip), ctx_kind::U64 },
    { "LastExceptionFromRip", offsetof(CONTEXT, LastExceptionFromRip), ctx_kind::U64 },
#endif
};

const ctx_field *context_fields(size_t *count)
{
    *count = sizeof(k_context_fields) / sizeof(k_context_fields[0]);
    return k_context_fields;
}

/* ------------------------------ 薄封装 ------------------------------ */
bool DebuggerSession::get_context(HANDLE hThread, DWORD flags, CONTEXT *out)
{
    ZeroMem(*out);
    out->ContextFlags = flags;
    if (!GetThreadContext(hThread, out))
    {
        E("GetThreadContext");
        return false;
    }
    return true;
}

bool DebuggerSession::set_context(HANDLE hThread, DWORD flags, const CONTEXT *in)
{
    /* 直接写回：调用方提供的必须是**完整** CONTEXT（通常先 get_context 取基准，
     * 再按宿主表仅改其中的字段）—— 与旧实现「先 Get 再按表覆盖」语义一致，
     * 但不重复一次取值。 */
    CONTEXT context = *in;
    context.ContextFlags = flags;
    if (!SetThreadContext(hThread, &context))
    {
        E("SetThreadContext");
        return false;
    }
    return true;
}

bool DebuggerSession::update_all_threads_context(DWORD flags, bool (*mutate)(CONTEXT *ctx, void *user), void *user)
{
    bool any = false;
    for (ThreadInfo *info = threads().head(); info; info = info->next)
    {
        CONTEXT context;
        context.ContextFlags = flags;
        if (!GetThreadContext(info->hThread, &context))
            continue; /* 旧行为：取不到就跳过该线程（不报错、不影响其它线程） */
        if (!mutate(&context, user))
            continue;
        SetThreadContext(info->hThread, &context);
        any = true;
    }
    return any;
}
}