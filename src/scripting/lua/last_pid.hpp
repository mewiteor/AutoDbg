#ifndef AUTODBG_SCRIPTING_LUA_LAST_PID_HPP
#define AUTODBG_SCRIPTING_LUA_LAST_PID_HPP
/* =============================================================================
 * src/scripting/lua/last_pid.hpp —— 「上次附加的 PID」持久化（宿主层）
 *
 * 来源：`tools.c` / `tools.h`（C，P3.B 模块 12 迁入并转 C++）。
 * 归属：**Lua 端**（Q7：`g_wfilepath` 与这类「与调试无关、将来 Python cffi 也不需要」的
 *   功能都不进 core）。存储位置 = 脚本路径的扩展名换成 `suffix`，例如
 *   `D:\x\a.lua` + `.attach.last.pid.bin` → `D:\x\a.attach.last.pid.bin`。
 *
 * 迁移时的机械修正（缺陷 **D2**，已在阶段 3 清单内）：`tools.c` 的两个函数都带一个
 *   `lua_State *L` 形参，**全程未使用** → 去掉；脚本路径改由调用方显式传入（调用点
 *   就是持有 `g_wfilepath` 的绑定层）。行为（含失败路径的报错文案）1:1 保留。
 * ========================================================================== */
#include <windows.h>

/* 读上次保存的 PID（文件不存在 / 读失败 / 长度不符 → 0，并打印与旧实现相同的错误） */
DWORD get_last_pid(LPCTSTR script_path, LPCTSTR suffix);

/* 写 PID（写失败只报错，不抛） */
void save_last_pid(LPCTSTR script_path, DWORD pid, LPCTSTR suffix);

#endif /* AUTODBG_SCRIPTING_LUA_LAST_PID_HPP */
