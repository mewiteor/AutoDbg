#ifndef AUTODBG_CORE_PE_HPP
#define AUTODBG_CORE_PE_HPP
/* =============================================================================
 * src/core/pe.hpp —— PE / 模块信息（P3.B 模块）
 *
 * 依赖会话的模块表（`DebuggerSession::peinfos()`；P6.4 后引擎函数即会话成员）。
 * 注：`get_peinfo_by_name` 以 `extern "C"` 声明（hide.cpp 也要用），
 *     这里保持同样的链接规范。
 * ========================================================================== */
#include "core.hpp"   /* P6.4：引擎函数已是 DebuggerSession 成员（下面的过渡转发也要它） */
#include "types.hpp"


/* 把 `\Device\HarddiskVolumeX\...` 形态的 NT 原生路径转成 Win32 路径（原地改写并返回新起点） */
LPCTSTR TranslateNativeName(LPTSTR nativeName);

/* 解析模块的 PE（导入/导出/重定位/异常目录等），填充 peinfo */



#endif /* AUTODBG_CORE_PE_HPP */
