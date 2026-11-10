#ifndef AUTODBG_CORE_HARDWARE_BREAKPOINTS_HPP
#define AUTODBG_CORE_HARDWARE_BREAKPOINTS_HPP
/* =============================================================================
 * src/core/hardware_breakpoints.hpp —— 硬件断点（P3.B 模块）
 *
 * 用调试寄存器 Dr0-Dr3（地址）+ Dr7（类型/长度使能）；`index` 为 dr 序号 0..3。
 * 语义与旧实现一致：把 Dr7 / Dr<index> 写进「已知线程表」（g_thread_list）
 *   里的每个线程；取不到某线程的 context 就跳过它。
 * 依赖：context（update_all_threads_context）+ 会话状态（硬件断点表 / 进程句柄）。
 * ========================================================================== */
#include "callback_token.h"
#include "core.hpp"   /* P6.4：引擎函数已是 DebuggerSession 成员（下面的过渡转发也要它） */
#include "types.hpp"

/* 登记一个硬件断点并写入各线程；callback 是宿主无关令牌（可为「无回调」） */


/* ❗ IndexOfRegister == 0 表示「全部 4 个」：清 Dr0-Dr3 并使登记表清零 */



#endif /* AUTODBG_CORE_HARDWARE_BREAKPOINTS_HPP */
