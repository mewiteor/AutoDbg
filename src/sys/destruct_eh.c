/* =============================================================================
 * src/sys/destruct_eh.c —— CRT 缺口：`__DestructExceptionObject`（Win9x 兼容）
 *
 * 文件职责
 *     为 thirdpart/luajit/src/lj_err.c 提供 `__DestructExceptionObject`。该符号是
 *     msvcrt.dll 的导出，但 **Win95/Win98 的 msvcrt.dll 没有它**（XP+ 才有）。
 *     静态导入会让 x86 产物在老系统上「映射即失败」（手工映射器解析导入是硬失败，
 *     见 src/sys/pack/module_loader.cpp）。
 *
 * 为什么不能像 strtoull 那样纯自实现
 *     它是 MSVC C++ 异常运行时的一部分（按 EXCEPTION_RECORD 析构异常对象），
 *     没有可移植的零 CRT 等价实现。
 *
 * 做法（与 src/sys/strtoull.c 同一口径：自己提供符号，导入表零新增）
 *     · 运行时向**已经加载**的 msvcrt.dll 取真正的实现并转发：
 *       XP+ 的行为与原静态导入完全一致；
 *     · 老系统取不到 → 降级为「不析构」：只可能漏掉一次 C++ 异常对象的析构
 *       （本项目禁 C++ 异常；LuaJIT 只会在 C++ 异常穿过 Lua 帧时才走到这条路径，
 *       而在 x86 上我们本来就走 LUAJIT_NO_UNWIND / setjmp 路径）。
 *
 * 与旧实现的差异
 *     新增文件（Win9x 导入表专项）；此前该引用由静态导入 msvcrt.dll 满足。
 *
 * 注：本文件属 src/sys/**，按约定**不带 BOM**；只用到 kernel32 的 GetModuleHandleA /
 *     GetProcAddress（均为 Win95 可用），不引入任何新导入。
 * ========================================================================== */
#include <windows.h>

typedef void(__cdecl *destruct_eh_fn)(EXCEPTION_RECORD *, int);

void __cdecl __DestructExceptionObject(EXCEPTION_RECORD *rec, int nothrow)
{
    /* 零初始化（零 CRT 下不存在「静态对象反注册」问题，也不参与 TLS） */
    static destruct_eh_fn real_fn;
    static int            resolved;

    if (!resolved)
    {
        /* msvcrt.dll 一定是已加载的（本模块静态导入它）→ 只查不加载 */
        HMODULE m = GetModuleHandleA("msvcrt.dll");
        real_fn   = m ? (destruct_eh_fn)(void *)GetProcAddress(m, "__DestructExceptionObject") : NULL;
        resolved  = 1;
    }
    if (real_fn)
        real_fn(rec, nothrow);
}
