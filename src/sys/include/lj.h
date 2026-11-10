#ifndef _LJ_H_
#define _LJ_H_

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifdef __cplusplus
extern "C"
{
#endif

    inline BOOL LjGetModuleHandleEx(LPCVOID addr, HMODULE *ph)
    {
        if (!ph)
            return FALSE;
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(addr, &mbi, sizeof mbi) != sizeof mbi)
            return FALSE;
        *ph = (HMODULE)mbi.AllocationBase;
        return TRUE;
    }

    /* ------------------------------- Win9x 垫片 -------------------------------
     * LuaJIT 直接调用 `GetModuleHandleExA`（Win2000+）→ Win9x 产物会静态导入它
     * （TODO 9.1 D10；导入表里一旦有 XP+ 符号，Win9x 上**加载**就失败）。
     * 本头在 LuaJIT/98 构建里被 `/FI` 强制包含，且在 `windows.h` **之后**：
     *   · SDK 对 `GetModuleHandleExA` 的 dllimport 声明不受影响（已处理过）；
     *   · 之后的**声明与调用**被对象宏改成 `Autodbg_GetModuleHandleExA`
     *     （实现见 `src/common/win9x_shim.c`）。
     * 用对象宏（而非函数宏）：否则 LuaJIT 里的原型声明会被展开成非法语法。
     *
     * 实现为什么是 `static __inline`（而不是 `src/common/win9x_shim.c` 里的外部符号）：
     * LuaJIT 的调用点（lj_clib.c / lib_package.c）被编进 `luajit\luajit.lib`，
     * 而这份静态库同时供 `luajit_<arch>.exe` 与 `AutoDbg_luajit_<arch>.exe` 链接。
     * 走外部符号的话，两个产物各自需要的提供者不在同一个链接单元里；改成 inline
     * 就没有外部引用，两边都自洽、也不需要额外的 .obj。
     * `AUTODBG_SHIM_INLINE` 让 `src/common/win9x_shim.c` 在已强制包含本头时退让
     * （避免同一 TU 里既有 static inline 又有外部定义）。 */
#if defined(AUTODBG_WIN9X)
    static __inline BOOL WINAPI Autodbg_GetModuleHandleExA(DWORD dwFlags, LPCSTR lpModuleName, HMODULE *phModule)
    {
        if (!phModule)
            return FALSE;

        if (dwFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS)
        {
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery((LPCVOID)lpModuleName, &mbi, sizeof mbi) != sizeof mbi)
                return FALSE;
            *phModule = (HMODULE)mbi.AllocationBase;
            return TRUE;
        }

        *phModule = GetModuleHandleA(lpModuleName);
        return *phModule != NULL;
    }
#define AUTODBG_SHIM_INLINE
#define GetModuleHandleExA Autodbg_GetModuleHandleExA
#endif

#ifdef __cplusplus
}
#endif

#endif // _LJ_H_
