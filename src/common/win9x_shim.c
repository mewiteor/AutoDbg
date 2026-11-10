/* =============================================================================
 * src/common/win9x_shim.c —— Win9x（`--98=y`）专用的系统 API 垫片
 *
 * 背景（TODO 9.1 D10 / Q24）：x86 的 Win9x 产物要求**导入表零 XP+ API**，否则在
 * Win9x 上「加载就失败」（不是运行到某分支才失败）。自有代码已全部走
 * `src/common/dynapi` 或非 Unicode shim，但**第三方 LuaJIT** 仍直接调用
 * `GetModuleHandleExA`（Win2000+）：
 *   thirdpart/luajit/src/lj_clib.c:255,257,261 与 lib_package.c:163
 * 第三方源码不改（规矩），改法是把**调用点**在编译期换成我们自己的实现：
 *   `src/sys/include/lj.h`（LuaJIT 构建强制包含）里
 *       #define GetModuleHandleExA Autodbg_GetModuleHandleExA
 * 名字不同是刻意的：SDK 的 `GetModuleHandleExA` 带 `__declspec(dllimport)`，
 * 直接用 API 名定义会报 C2491（定义 dllimport 函数）；换了名字才能在我们的 TU 里定义。
 *
 * 现状：实现已经搬进 `src/sys/include/lj.h` 的 `static __inline`
 * （LuaJIT 的调用点被编进 `luajit.lib`，外部符号在两个产物之间没有共同的提供者）。
 * 本文件因此在该头被 /FI 强制包含时退让（`AUTODBG_SHIM_INLINE`），
 * 只在「没强制包含 lj.h、却又要这个垫片」的构建里提供外部符号，避免重复定义。
 *
 * 语义：LuaJIT 只用两种形态，按 WINAPI 的约定把它们做对即可
 *   · `FROM_ADDRESS | UNCHANGED_REFCOUNT` + 代码地址 → 该地址所属模块
 *   · `UNCHANGED_REFCOUNT` + NULL              → 主模块（EXE）
 * `UNCHANGED_REFCOUNT` 表示「不增加引用计数」，正好与我们不持有引用一致。
 * ========================================================================== */
#include <windows.h>

#if defined(AUTODBG_WIN9X) && defined(JIT) && !defined(AUTODBG_SHIM_INLINE)
/* 只处理 Win9x + LuaJIT 组合：现代产物有真 API，Lua 5.5 产物根本不用它（避免死代码） */
BOOL WINAPI Autodbg_GetModuleHandleExA(DWORD dwFlags, LPCSTR lpModuleName, HMODULE *phModule)
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
#endif /* AUTODBG_WIN9X && JIT */
