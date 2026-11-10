/* =============================================================================
 * src/core/hardware_breakpoints.cpp —— 硬件断点（P3.B 模块拆分，逐字从 bindings.cpp 迁入）
 *
 * 内容：set_hardware_breakpoint / enable_hardware_breakpoint / disable_hardware_breakpoint /
 *       del_hardware_breakpoint + 3 个线程级 mutate 函数（HwbpThreadParam 配套）。
 *
 * 跨模块改动（仅这些）：
 *   · set_hardware_breakpoint 去掉 static（bindings.cpp 的 lua_ 封装要调）；
 *   · 「对每个已知线程 取context → 改 → 写回」的循环改用
 *     core::update_all_threads_context（模块 4 抽出的唯一一处），
 *     旧形态是 4 个函数各抄一遍。
 *   · 令牌已经是 callback_ref_t（宿主无关、按值），本模块不碰 Lua。
 * 依赖：context（update_all_threads_context）+ 会话状态（硬件断点表 / 进程句柄）。
 * ========================================================================== */
#include "hardware_breakpoints.hpp"
#include "context.hpp"
#include "core.hpp" /* P6.4：引擎函数是会话成员（本 TU 多数定义即成员，直接用自身状态） */
#include "types.hpp"
#include "util.hpp"

namespace core = autodbg::core;   /* 与 bindings.cpp 一致：显式限定，避免与 C 头的同名类型二义 */
using autodbg::core::DebuggerSession; /* P6.4：引擎函数已是它的成员 */

#include <assert.h>
#include <intrin.h>
struct HwbpThreadParam
{
    DWORD  dr7;
    DWORD  index; /* dr 序号 0..3 */
    size_t ea;
};

/* 站点 1/2 共用：写 Dr7 → 把 Dr<index> 设为 ea（站点 2 传的 ea 取自登记表） */
static bool hwbp_mutate_set(CONTEXT *c, void *user)
{
    HwbpThreadParam *p = (HwbpThreadParam *)user;
    c->Dr7             = p->dr7;
    switch (p->index)
    {
    case 0: c->Dr0 = p->ea; break;
    case 1: c->Dr1 = p->ea; break;
    case 2: c->Dr2 = p->ea; break;
    case 3: c->Dr3 = p->ea; break;
    }
    return true;
}

/* 站点 3（Disable）：清 Dr<index> */
static bool hwbp_mutate_disable(CONTEXT *c, void *user)
{
    HwbpThreadParam *p = (HwbpThreadParam *)user;
    c->Dr7             = p->dr7;
    (&c->Dr0)[p->index] = 0;
    return true;
}

/* 站点 4（Del）：index==0 表示清全部 Dr0..Dr3 */
static bool hwbp_mutate_del(CONTEXT *c, void *user)
{
    HwbpThreadParam *p = (HwbpThreadParam *)user;
    c->Dr7             = p->dr7;
    if (!p->index)
        ZeroMemN(&c->Dr0, 4);
    else
        (&c->Dr0)[p->index - 1] = 0;
    return true;
}

bool DebuggerSession::set_hardware_breakpoint(size_t ea, DWORD Index_of_register, char bpx_type, DWORD bpx_size, callback_ref_t callback)
{
    int hwbpSize;
    /* ❗D32 修复：DR7 的 LEN 编码是 00=1B、01=2B、**11=4B**、10=8B（x64 专属）——
     * 旧代码把 4 和 8 写反了（4→10、8→11）→ size=4 实际拿到 8 字节窗口（会连带命中相邻存储），
     * 也是「数据监视命中时机漂移」的根因之一。 */
    switch (bpx_size)
    {
    case 1: hwbpSize = 0; break;
    case 2:
        hwbpSize = 1;
        if ((ea % 2) != 0)
            return false;

        break;
    case 4:
        hwbpSize = 3;
        if ((ea % 4) != 0)
            return false;
        break;
    case 8:
        hwbpSize = 2;
        if ((ea % 8) != 0)
            return false;
        break;
    default: return false;
    }

    if (!Index_of_register)
    {
        if (!hw_breakpoints()[0].activate)
            Index_of_register = 1;
        else if (!hw_breakpoints()[1].activate)
            Index_of_register = 2;
        else if (!hw_breakpoints()[2].activate)
            Index_of_register = 3;
        else if (!hw_breakpoints()[3].activate)
            Index_of_register = 4;
        else
            return false;
    }

    int hwbpIndex = -1;
    if (0 < Index_of_register && Index_of_register <= 4)
        hwbpIndex = Index_of_register - 1;
    else
        return false;

    struct
    {
        BYTE mode_0: 2;
        BYTE mode_1: 2;
        BYTE mode_2: 2;
        BYTE mode_3: 2;
        BYTE unused;
        BYTE type_0: 2;
        BYTE size_0: 2;
        BYTE type_1: 2;
        BYTE size_1: 2;
        BYTE type_2: 2;
        BYTE size_2: 2;
        BYTE type_3: 2;
        BYTE size_3: 2;
    } dr7;

    CONTEXT context;
    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(proc().hThread, &context))
        return false;
    *(PDWORD)&dr7 = (DWORD)context.Dr7;

    int hwbpType;
    switch (bpx_type)
    {
    case 'e':
        hwbpSize = 0; // size 1
        hwbpType = 0;
        break;
    case 'w': hwbpType = 1; break;
    case 'r': hwbpType = 3; break;
    default : return false;
    }

    switch (hwbpIndex)
    {
    case 0:
        dr7.mode_0 = 1; // local
        dr7.size_0 = hwbpSize;
        dr7.type_0 = hwbpType;
        break;
    case 1:
        dr7.mode_1 = 1; // local
        dr7.size_1 = hwbpSize;
        dr7.type_1 = hwbpType;
        break;
    case 2:
        dr7.mode_2 = 1; // local
        dr7.size_2 = hwbpSize;
        dr7.type_2 = hwbpType;
        break;
    case 3:
        dr7.mode_3 = 1; // local
        dr7.size_3 = hwbpSize;
        dr7.type_3 = hwbpType;
        break;
    }

    HwbpThreadParam p = {*(PDWORD)&dr7, (DWORD)hwbpIndex, ea};
    this->update_all_threads_context(CONTEXT_DEBUG_REGISTERS, hwbp_mutate_set, &p);

    hw_breakpoints()[hwbpIndex].DrxBreakPointType = bpx_type;
    hw_breakpoints()[hwbpIndex].DrxBreakPointSize = bpx_size;
    hw_breakpoints()[hwbpIndex].activate          = 1;
    hw_breakpoints()[hwbpIndex].ea                = ea;
    hw_breakpoints()[hwbpIndex].callback          = callback;

    return true;
}

void DebuggerSession::enable_hardware_breakpoint(DWORD IndexOfRegister)
{
    if (IndexOfRegister >= 4)
        return;
    if (!hw_breakpoints()[IndexOfRegister].ea)
        return;
    int hwbpSize;
    /* 同 `set_hardware_breakpoint`：LEN 编码 00=1B、01=2B、11=4B、10=8B（x64）—— 见 D32 */
    switch (hw_breakpoints()[IndexOfRegister].DrxBreakPointSize)
    {
    case 1: hwbpSize = 0; break;
    case 2: hwbpSize = 1; break;
    case 4: hwbpSize = 3; break;
    case 8: hwbpSize = 2; break;
    }

    struct
    {
        BYTE mode_0: 2;
        BYTE mode_1: 2;
        BYTE mode_2: 2;
        BYTE mode_3: 2;
        BYTE unused;
        BYTE type_0: 2;
        BYTE size_0: 2;
        BYTE type_1: 2;
        BYTE size_1: 2;
        BYTE type_2: 2;
        BYTE size_2: 2;
        BYTE type_3: 2;
        BYTE size_3: 2;
    } dr7;

    CONTEXT context;
    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(proc().hThread, &context))
        return;
    *(PDWORD)&dr7 = (DWORD)context.Dr7;

    int hwbpType;
    switch (hw_breakpoints()[IndexOfRegister].DrxBreakPointType)
    {
    case 'e':
        hwbpSize = 0; // size 1
        hwbpType = 0;
        break;
    case 'w': hwbpType = 1; break;
    case 'r': hwbpType = 3; break;
    }

    switch (IndexOfRegister)
    {
    case 0:
        dr7.mode_0 = 1; // local
        dr7.size_0 = hwbpSize;
        dr7.type_0 = hwbpType;
        break;
    case 1:
        dr7.mode_1 = 1; // local
        dr7.size_1 = hwbpSize;
        dr7.type_1 = hwbpType;
        break;
    case 2:
        dr7.mode_2 = 1; // local
        dr7.size_2 = hwbpSize;
        dr7.type_2 = hwbpType;
        break;
    case 3:
        dr7.mode_3 = 1; // local
        dr7.size_3 = hwbpSize;
        dr7.type_3 = hwbpType;
        break;
    }

    HwbpThreadParam p = {*(PDWORD)&dr7, IndexOfRegister, hw_breakpoints()[IndexOfRegister].ea};
    this->update_all_threads_context(CONTEXT_DEBUG_REGISTERS, hwbp_mutate_set, &p);

    hw_breakpoints()[IndexOfRegister].activate = 1;
}

void DebuggerSession::disable_hardware_breakpoint(DWORD IndexOfRegister)
{
    DWORD dr7;
    if (IndexOfRegister >= 4)
        return;
    CONTEXT context;
    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(proc().hThread, &context))
    {
        dr7 = (DWORD)context.Dr7;
        dr7 &= ~(3 << IndexOfRegister * 2);
        dr7 &= ~(0xF << (IndexOfRegister * 4 + 16));
    }
    HwbpThreadParam p = {*(PDWORD)&dr7, IndexOfRegister, 0};
    this->update_all_threads_context(CONTEXT_DEBUG_REGISTERS, hwbp_mutate_disable, &p);
    hw_breakpoints()[IndexOfRegister].activate = 0;
}

void DebuggerSession::del_hardware_breakpoint(DWORD IndexOfRegister)
{
    DWORD dr7;
    if (IndexOfRegister > 4)
        return;
    *(PDWORD)&dr7 = 0;
    if (IndexOfRegister)
    {
        CONTEXT context;
        context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(proc().hThread, &context))
        {
            dr7 = (DWORD)context.Dr7;
            dr7 &= ~(3 << (IndexOfRegister - 1) * 2);
            dr7 &= ~(0xF << ((IndexOfRegister - 1) * 4 + 16));
        }
    }
    HwbpThreadParam p = {*(PDWORD)&dr7, IndexOfRegister, 0};
    this->update_all_threads_context(CONTEXT_DEBUG_REGISTERS, hwbp_mutate_del, &p);
    if (IndexOfRegister)
        ZeroMem(hw_breakpoints()[IndexOfRegister - 1]);
    else
        ZeroMem(hw_breakpoints());
}
