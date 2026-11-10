#ifndef AUTODBG_CORE_BREAKPOINTS_HPP
#define AUTODBG_CORE_BREAKPOINTS_HPP
/* =============================================================================
 * src/core/breakpoints.hpp —— 软件断点（P3.B 模块）
 *
 * 依赖 memory / pe / engine（`g_breakpoint_list`）。
 * 约定：断点自带 callback_ref_t 令牌（宿主无关、按值，见 callback_token.h）；删除断点时通过
 *       `DebugCallbacks::on_breakpoint_destroyed` 通知调用方释放令牌。
 * ========================================================================== */
#include "core.hpp"   /* P6.4：引擎函数已是 DebuggerSession 成员（下面的过渡转发也要它） */
#include "types.hpp"   /* autodbg::core::DebugCallbacks / PEInfo（类型统一后只有这一份） */

/* modname：模块名（大小写不敏感）；rva：模块内偏移；is_single：命中一次即删；
 * callback：调用方令牌（宿主无关，按值；callback.valid = 0 表示无回调，见 callback_token.h） */

/* cb 可为 NULL（调用方自己负责释放令牌）；否则对每个被删断点回调 on_breakpoint_destroyed */


/* 把该模块上登记的断点写进内存（0xCC）；不含 PE 解析 */

/* 解析模块 PE 并把它上面登记的所有断点写进内存（0xCC） */

/* 删掉某模块上的全部断点（模块卸载 / 进程退出时用），不恢复内存 */



#endif /* AUTODBG_CORE_BREAKPOINTS_HPP */
