/* =============================================================================
 * src/core/breakpoints.cpp —— 软件断点（P3.B 模块拆分，逐字从 bindings.cpp 迁入）
 *
 * 内容：set_breakpoint / del_breakpoint / enable_breakpoint / disable_breakpoint /
 *       update_breakpoints / apply_breakpoints_for_module /
 *       del_breakpoints_by_modname_without_update_memory
 *
 * 跨模块改动（仅这些）：
 *   · set_breakpoint / update_breakpoints 去掉 static（bindings.cpp 调用）；
 *   · del_breakpoint 的 lua_State *L → const core::DebugCallbacks *cb —— 旧代码在此
 *     直接 lua_unref，现改为走销毁通知（令牌由调用方释放，见 Plan 5.5）。
 *   · C2b：`apply_breakpoints_for_module` 的读写改走 `peinfo->dwProcessId` 的句柄
 *     （进程树里子进程的断点补丁不能打到根进程）。
 * 依赖：memory（读写）、pe（get_peinfo_* 与 parse_pe）、engine（g_breakpoint_list）。
 * ========================================================================== */
#include "breakpoints.hpp"
#include "core.hpp" /* P6.4：引擎函数是会话成员（本 TU 多数定义即成员，直接用自身状态） */
#include "types.hpp"
#include "memory.hpp"
#include "pe.hpp"
#include "util.hpp"

namespace core = autodbg::core;   /* 与 bindings.cpp 一致：显式限定，避免与 C 头的同名类型二义 */
using autodbg::core::DebuggerSession; /* P6.4：引擎函数已是它的成员 */

#include <assert.h>
#include <intrin.h>

/* 类型统一（删 legacy_types.h）：这些名字现在只有 types.hpp 里的 `autodbg::core::` 那套 */
using autodbg::core::PEInfo;
using autodbg::core::BreakpointInfo;
void DebuggerSession::set_breakpoint(LPCTSTR modname, DWORD rva, bool is_single, callback_ref_t callback)
{
    for (BreakpointInfo *info = breakpoints().head(); info; info = info->next)
    {
        if (rva == info->rva && !_tcsicmp(modname, info->modname))
        {
            info->is_single = is_single;
            info->callback = callback;
            return;
        }
    }
    size_t                 n    = _tcslen(modname);
    /* P6.2：变长节点（`modname[1]` 尾数组）由链表一次分配（= 旧 `alloc_(sizeof + n*sizeof(TCHAR))`） */
    BreakpointInfo *info = breakpoints().create_front(sizeof(TCHAR) * n);
    CopyMem(info->modname, modname, sizeof(TCHAR) * n);
    info->modname[n]       = 0;
    info->rva              = rva;
    info->callback         = callback;
    info->is_single        = is_single ? 1 : 0;
    info->orignal_byte     = 0;
    info->has_orignal_byte = 0;
    info->activate         = 1;

    size_t base = get_base(modname);
    BYTE   int3 = 0xCC;
    if (base && update_memory(base + rva, &info->orignal_byte, &int3, 1, true))
        info->has_orignal_byte = 1;
}

void DebuggerSession::del_breakpoint(const core::DebugCallbacks *cb, size_t ea, LPCTSTR modname, size_t rva)
{
    for (BreakpointInfo *tmp = breakpoints().head(); tmp; )
    {
        BreakpointInfo *next = tmp->next; /* 先留 next（remove 会置空） */
        if (tmp->rva != rva || _tcsicmp(modname, tmp->modname))
        {
            tmp = next;
            continue;
        }
        if (tmp->activate && tmp->has_orignal_byte)
            update_memory(ea, NULL, &tmp->orignal_byte, 1, true);
        if (cb && cb->on_breakpoint_destroyed && tmp->callback.valid)
            cb->on_breakpoint_destroyed(cb->context, tmp->callback);
        breakpoints().remove_and_free(tmp);
        tmp = next;
    }
}

/* ❗ 去 Lua 化后它的职责是：删断点 + **逐个通知**回调销毁（令牌由调用方释放）。
 * `cb` 可为空（Lua 主动调用的路径会自己把 ref 清掉，传 NULL）。 */
void DebuggerSession::del_breakpoints_by_modname_without_update_memory(const core::DebugCallbacks *cb, LPCTSTR modname)
{
    for (BreakpointInfo *tmp = breakpoints().head(); tmp; )
    {
        BreakpointInfo *next = tmp->next;
        if (_tcsicmp(modname, tmp->modname))
        {
            tmp = next;
            continue;
        }
        if (cb && cb->on_breakpoint_destroyed && tmp->callback.valid)
            cb->on_breakpoint_destroyed(cb->context, tmp->callback);
        breakpoints().remove_and_free(tmp);
        tmp = next;
    }
}

/**
 * apply_breakpoints_for_module - 为模块应用所有断点
 * @param modname 模块名
 * @param base 模块基址
 *
 * ❗T7 切片 C2b（plan_t7.md §10 第 3 条）：读写一律走 **`peinfo->dwProcessId`** 的句柄。
 *   旧实现直取 `proc().hProcess`（根进程）⇒ 在进程树里，子进程的 `CREATE_PROCESS` / `LOAD_DLL`
 *   会拿根进程的句柄去改子进程的代码页：要么改错地方、要么改失败（断点记录有，补丁没打上）。
 *   断点记录的 key 仍是 `(modname, rva)`（`BreakpointInfo` 无 pid 且布局不可动）⇒ 同名模块在
 *   树里每个进程各打一份 `0xCC`，这是本切片的已有口径（“断点的进程维度落在新记录层”留给后续）。
 */
void DebuggerSession::apply_breakpoints_for_module(PEInfo *peinfo)
{
    if (!peinfo->sections || !peinfo->section_count)
        return;
    for (int i = 0; i < peinfo->section_count; i++)
    {
        IMAGE_SECTION_HEADER *sec = &peinfo->sections[i];
        if (!(sec->Characteristics & 0x20000000))
            continue; // IMAGE_SCN_MEM_EXECUTE
        size_t sec_va   = sec->VirtualAddress;
        size_t sec_size = sec->Misc.VirtualSize > 0 ? sec->Misc.VirtualSize : sec->SizeOfRawData;
        size_t sec_addr = peinfo->base + sec_va;

        bool has_bp = false;
        for (BreakpointInfo *bpinfo = breakpoints().head(); bpinfo; bpinfo = bpinfo->next)
        {
            if (!bpinfo->activate || bpinfo->has_orignal_byte)
                continue;
            if (bpinfo->rva < sec_va || bpinfo->rva >= sec_va + sec_size)
                continue;
            if (_tcsicmp(bpinfo->modname, peinfo->Path + peinfo->name_offset))
                continue;
            has_bp = true;
            break;
        }
        if (!has_bp)
            continue;

        BYTE *sec_buf = AllocX(BYTE, sec_size);
        if (!read(peinfo->dwProcessId, sec_addr, sec_buf, sec_size))
            continue;
        for (BreakpointInfo *info = breakpoints().head(); info; info = info->next)
        {
            if (!info->activate)
                continue;
            if (info->rva < sec_va || info->rva >= sec_va + sec_size)
                continue;
            if (_tcsicmp(info->modname, peinfo->Path + peinfo->name_offset))
                continue;
            size_t offset          = info->rva - sec_va;
            info->orignal_byte     = sec_buf[offset];
            sec_buf[offset]        = 0xCC;
            info->has_orignal_byte = 1;
        }
        write(peinfo->dwProcessId, sec_addr, sec_buf, sec_size, true);
    }
}

void DebuggerSession::update_breakpoints(PEInfo *peinfo)
{
    // 解析PE文件获取节区信息
    parse_pe(peinfo);
    // 应用断点
    apply_breakpoints_for_module(peinfo);
}

void DebuggerSession::disable_breakpoint(size_t ea, LPCTSTR modname, DWORD rva)
{
    // 设置断点激活标志为0
    for (BreakpointInfo *info = breakpoints().head(); info; info = info->next)
        if (info->rva == rva && !_tcsicmp(modname, info->modname) && info->activate && info->has_orignal_byte && write(ea, &info->orignal_byte, 1, true))
        {
            info->activate         = 0;
            info->has_orignal_byte = false;
            info->orignal_byte     = 0;
        }
}

void DebuggerSession::enable_breakpoint(size_t ea, LPCTSTR modname, DWORD rva)
{
    // 设置断点激活标志
    for (BreakpointInfo *info = breakpoints().head(); info; info = info->next)
    {
        if (info->has_orignal_byte)
            continue;
        if (info->activate)
            continue;
        if (info->rva == rva && !_tcsicmp(modname, info->modname) && !info->activate)
        {
            BYTE int3 = 0xCC;
            if (update_memory(ea, &info->orignal_byte, &int3, 1, true))
            {
                info->has_orignal_byte = 1;
                info->activate         = 1;
            }
        }
    }
}