/* =============================================================================
 * src/scripting/lua/main.cpp —— 进程入口与 CRT 兼容层（P4.7，从 bindings.cpp 搬出）
 *
 * 内容：会话生命周期（`s_session`）、`_tmain`（选脚本 + 建状态跑脚本）、
 *   `run_once`（宿主无关的「跑一次」：isa 初始化 → init_tab → _tmain → 收尾 → puts("done")）、
 *   `autodbg_run`（**模块 DLL 的唯一导出**，Plan §2.3 / P3）、
 *   `mainCRTStartup`（`/ENTRY` 指定的入口，仍用于旧 exe 构建）、`__tgetmainargs`、
 *   `__isa_available_init`（x86 的 asm 侧符号）、`InitCrtDbg`/`CheckMemLeak`（仅 _DEBUG）。
 * ❗这些符号必须保持 **C 链接**（`/ENTRY:mainCRTStartup` 与 msvcrt.def 都按 C 名字查找）。
 * 注：真正「建 Lua 状态 → 注册 → 加载并运行脚本」在 `state.cpp`（P4.1）。
 *
 * P3 改动（与旧实现的差异）：
 *   ① 新增 `run_once()`，把「isa 初始化 + init_tab + _tmain + 会话释放」从 `mainCRTStartup` 里提出来，
 *      让 exe 入口与 DLL 导出共用一个实现（不复制两份逻辑）；
 *   ② 新增 `autodbg_run(const autodbg_boot *)`：替代 `mainCRTStartup` 里的
 *      `__getmainargs` + `ExitProcess`（用 `boot.cmdline_*` + `autodbg_split_cmdline_*`），
 *      并用 `boot.script_*` 覆盖 `argv[1]`；
 *   ③ 文件对话框逻辑**保留**（旧 exe 的行为不变），但启动器总会传脚本 → DLL 侧不会弹窗（P4 再搬到启动器）。
 * ========================================================================== */
#include <windows.h>

#include "core/core.hpp" /* DebuggerSession + set_current_session */
#include "core/util.hpp" /* E / ErrorS / Error / ErrorN / macro helpers */
#include "state.hpp"     /* run_script_file / g_wfilepath */
#include "server.hpp"    /* S0：server 模式入口（plan_server.md §3.1 / D27） */
#include "common/dynapi.hpp" /* get_proc_as：Win95 缺失的 kernel32 API 动态解析 */
#include "common/selftest.hpp" /* S0：--selftest 纯函数自测（§11 验收） */

#include "../../common/cmdline.h"  /* autodbg_split_cmdline_a/w（P3） */
#include "../../sys/pack/boot.h"   /* autodbg_boot / AUTODBG_ABI_VERSION（P3） */

#include <assert.h>
#include <commdlg.h>
#include <intrin.h>
#include <shlwapi.h>
#include <strsafe.h>

#define _CRTDBG_MAP_ALLOC
#include <crtdbg.h>

using autodbg::core::Error;
using autodbg::core::ErrorN;
using autodbg::core::ErrorS;
using autodbg::core::init_tab; /* CRC 表（原 lib.c → core/util） */

/* P3.B 模块 11：引擎状态（原 8 个 `g_*` 全局量）已收进 `DebuggerSession`。
 * 本层是**宿主**：自己持有一个会话实例，并在启动时挂给 core（`set_current_session`）。
 * 会话成员通过 `core/core.hpp` 的访问器（`current_session().proc()` 等）显式访问（P6.4 已删 `engine.hpp` 宏别名层）；
 * 将来多实例（Plan 附录 C / P6.4）就是把这里换成一个会话表 + 显式传参。
 *
 * ❗为什么是指针 + `new` 而不是普通静态对象：零 CRT 构建（`/nodefaultlib`，msvcrt.def）
 *   **没有 `_atexit`**，带非平凡析构的静态对象会让链接器要 `_atexit`（LNK2019）。
 *   会话在进程生命期内常驻、退出时随进程释放（与旧实现同为“不释放链表”，行为中性）。 */
static autodbg::core::DebuggerSession *s_session = nullptr;

int _tmain(int argc, LPTSTR argv[])
{
    /* P3.B 模块 11：把本层的会话实例挂给 core（引擎函数用 current_session() 取状态）。
     * 必须在任何引擎调用之前完成。 */
    s_session = new autodbg::core::DebuggerSession();
    autodbg::core::set_current_session(s_session);

    TCHAR buf[MAX_FILEPATH];
    if (argc >= 2)
        g_wfilepath = argv[1];
    else
    {
        OPENFILENAME ofn;
        __stosb((PBYTE)&ofn, 0, sizeof ofn);
        ofn.lStructSize = sizeof ofn;
        ofn.lpstrFilter = TEXT("lua脚本\0*.lua\0");
        ofn.lpstrFile   = buf;
        ofn.nMaxFile    = _countof(buf);
        ofn.lpstrTitle  = TEXT("打开自动调试lua脚本");
        ofn.lpstrDefExt = TEXT(".lua");
        ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NONETWORKBUTTON;
        if (!GetOpenFileName(&ofn))
        {
            if (!GetLastError())
                return 0;
            E("GetOpenFileName");
            return -1;
        }
        g_wfilepath = buf;
    }
    if (!PathFileExists(g_wfilepath))
    {
        ErrorS(TEXT("文件不存在: "));
        Error(g_wfilepath);
        ErrorS("\n");
        return -1;
    }
    /* P4.1：建状态 → 注册 → 烘焙库 → 加载并运行脚本（原 `_tmain` 后半段，现 state.cpp） */
    return run_script_file(argc, argv);
}

/* P3.B 模块 11：原来的 `init()` / `release()` 已并入 `DebuggerSession` 的构造/析构
 * （`src/core/core.cpp`）—— 它们原先就是「把三个链表头置空 / 按序释放三个链表」，
 * 与 session 的 ctor/dtor 逐字一致。`mainCRTStartup` 改为 `new`/`delete` 会话实例。 */

#ifdef _DEBUG
static void InitCrtDbg(_CrtMemState *before)
{
    int flag = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);

    flag |= _CRTDBG_ALLOC_MEM_DF;    // 启用 debug 堆分配
    flag |= _CRTDBG_LEAK_CHECK_DF;   // 程序退出时自动检查泄漏
    flag |= _CRTDBG_CHECK_ALWAYS_DF; // 每次分配/释放检查堆，速度较慢，测试可用

    _CrtSetDbgFlag(flag);

    // 输出到控制台和调试器
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDOUT);

    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDOUT);

    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDOUT);

    _CrtMemCheckpoint(before);

    // 如果泄漏报告里看到类似 {156} 的分配编号，
    // 可以用下面这行在对应分配处断下：
    // _CrtSetBreakAlloc(156);
}

static void CheckMemLeak(_CrtMemState *before)
{
    _CrtMemState after, diff;
    _CrtMemCheckpoint(&after);

    if (_CrtMemDifference(&diff, before, &after))
    {
        printf("Memory leak detected between checkpoints.\n");

        _CrtMemDumpStatistics(&diff);
        _CrtMemDumpAllObjectsSince(before);
    }
}
#else
#   define InitCrtDbg(x)   ((void)x)
#   define CheckMemLeak(x) ((void)x)
#endif

#ifndef _WIN64
/* 这两个符号由 asm 侧（src/sys/flt.asm）以 C 名字 ___isa_available / ___isa_enabled 导出
 * → **必须 C 链接**（链接规范只能在全局作用域写，所以提到函数外面） */
extern "C" int __isa_available;
extern "C" int __isa_enabled;

int __isa_available_init()
{
    __isa_available = 0;
    __isa_enabled |= 1;
    /* ❗`IsProcessorFeaturePresent` 不在 Win95 的 kernel32 里（静态导入 = 映射硬失败）→
     * 动态解析；取不到就当作「没有 XSAVE」= 不启用 AVX 分派——正是老机器的正确语义
     * （`flt.asm` 的 `__ftol2`/`__ltod3` 等按 `__isa_available` 选 x87 还是 SSE）。 */
    typedef BOOL(WINAPI * IsProcessorFeaturePresentT)(DWORD);
    IsProcessorFeaturePresentT pIsProcessorFeaturePresent =
        autodbg::common::get_proc_as<IsProcessorFeaturePresentT>("kernel32.dll", "IsProcessorFeaturePresent");
    if (!pIsProcessorFeaturePresent || !pIsProcessorFeaturePresent(10))
        return 0;
    int var_24[4];
    __cpuidex(var_24, 0, 0);
    int var_C = var_24[0];
    __cpuidex(var_24, 1, 0);

    int __favor = 0;
    if (!(var_24[3] ^ 0x49656E69 | var_24[2] ^ 0x6C65746E | var_24[1] ^ 0x756E6547))
    {
        switch (var_24[0] & 0xFFF3FF0)
        {
        case 0x106C0:
        case 0x20660:
        case 0x20670:
        case 0x30650:
        case 0x30660:
        case 0x30670: __favor |= 1; break;
        }
    }
    int var_4 = var_24[2];
    var_24[1] = 0;
    if (var_C >= 7)
    {
        __cpuidex(var_24, 7, 0);
        if (var_24[1] & 0x200)
            __favor |= 2;
    }
    __isa_available = 1;
    __isa_enabled |= 2;
    if (var_4 & 0x100000)
    {
        __isa_available = 2;
        __isa_enabled |= 4;
        if ((var_4 & 0x18000000) == 0x18000000)
        {
            unsigned long long var_14 = _xgetbv(0);
            if ((var_14 & 6) == 6)
            {
                __isa_enabled |= 8;
                __isa_available = 3;
                if (var_24[1] & 0x20)
                {
                    __isa_enabled |= 0x20;
                    __isa_available = 5;
                    if ((var_24[1] & 0xD0030000) == 0xD0030000 && (var_14 & 0xE0) == 0xE0)
                    {
                        __isa_enabled |= 0x40;
                        __isa_available = 6;
                    }
                }
            }
        }
    }
    return 0;
}
#endif

#ifdef _UNICODE
#   define __tgetmainargs __wgetmainargs
#else
#   define __tgetmainargs __getmainargs
#endif
/* 注：入口与 CRT 兼容层必须保持 **C 链接**（/ENTRY:mainCRTStartup 与 msvcrt.def
 * 都按 C 名字查找） */
extern "C" int __tgetmainargs(int *, LPTSTR **, LPTSTR **, int, int *);

/* =============================================================================
 * run_once —— 宿主无关的「跑一次」（P3 新提出来，原为 mainCRTStartup 内联体）
 *
 * 顺序与旧 `mainCRTStartup` 逐字一致：isa 初始化 → init_tab → _tmain → 释放会话 → puts("done")。
 * 不含 `ExitProcess`、不含控制台 CP 切换（那两件事归调用方：exe 入口做 ExitProcess，
 * DLL 的 `autodbg_run` 把返回值交给启动器）。
 * ========================================================================== */
static int run_once(int argc, LPTSTR argv[])
{
#ifndef _WIN64
    __isa_available_init();
#endif
    init_tab();

    int r = _tmain(argc, argv);

    /* 会话生命周期收尾（= 旧 `release()`：PE → 线程 → 断点 三个链表的释放）。
     * 旧代码在 `_tmain` 前调 `init()`，现由 `_tmain` 里的构造取代。 */
    delete s_session;
    s_session = nullptr;
    autodbg::core::set_current_session(nullptr);

    puts("done");
    return r;
}

/* =============================================================================
 * run_selftest_mode —— S0：纯函数自测（`--selftest`，plan_server.md §11）
 *
 * 不需要脚本、不需要映射图像之外的东西；把 `common::run_selftests` 的每行报告
 * 走 `Print`（因此也会被 server 的输出 sink 捕获，口径统一）。
 * 返回 0 = 全通过；1 = 有失败。
 * ========================================================================== */
static void selftest_report(void *ctx, const char *line)
{
    (void)ctx;
    autodbg::core::PrintS(line);
    autodbg::core::PrintS("\n");
}

static int run_selftest_mode()
{
    int failed = autodbg::common::run_selftests(selftest_report, nullptr);

    if (failed == 0)
        autodbg::core::PrintS("selftest: PASS\n");
    else
        autodbg::core::ErrorS("selftest: FAIL\n");
    return (failed == 0) ? 0 : 1;
}

/* =============================================================================
 * autodbg_run —— 模块 DLL 的**唯一导出**（Plan §2.3 / P3）
 *
 * 等价于旧 `mainCRTStartup` 去掉 `__getmainargs` 与 `ExitProcess`：
 *   ① 校验 `boot->abi` / `boot->size`（不匹配就拒绝，而不去读半个结构）；
 *   ② 从 `boot->cmdline_a/w` 自己拆 argv（映射后的模块拿不到进程参数）；
 *   ③ `boot->script_a/w` 非空则覆盖 `argv[1]`（启动器已经确定好脚本了）；
 *   ④ 调 `run_once`，返回值即退出码（由启动器负责 ExitProcess）。
 * ========================================================================== */
extern "C" __declspec(dllexport) int __cdecl autodbg_run(const autodbg_boot *boot)
{
    if (boot == nullptr || boot->abi != AUTODBG_ABI_VERSION || boot->size != sizeof(autodbg_boot))
    {
        ErrorS("autodbg_run: boot ABI mismatch\n");
        return -1;
    }

    /* ---- S0（plan_server.md D27）：这两个模式都不需要脚本 → 在拆命令行之前分流 ---- */
    if (boot->selftest != 0)
        return run_selftest_mode();
    if (boot->server_mode != 0)
        return autodbg::server::server_main(boot);

    /* ---- 拆命令行（缓冲由我们自己持有：拆分是原地改写） ---- */
    static LPTSTR s_argv[64];
    int           argc = 0;

#ifdef _UNICODE
    /* Unicode 构建（= modern 变体）：优先用宽字符命令行；Win9x 上 `cmdline_w` 为 NULL，
     * 但 Win9x 走的是 legacy 变体（ANSI 构建），不会进这个分支。 */
    if (boot->cmdline_w == nullptr)
    {
        ErrorS("autodbg_run: missing wide command line\n");
        return -1;
    }
    {
        unsigned len = 0;
        while (boot->cmdline_w[len] != 0)
            len++;

        wchar_t *buf = (wchar_t *)autodbg::common::alloc((len + 1) * sizeof(wchar_t));
        for (unsigned i = 0; i <= len; i++)
            buf[i] = boot->cmdline_w[i];

        argc = autodbg_split_cmdline_w(buf, s_argv, (int)(sizeof s_argv / sizeof s_argv[0]));
        if (argc < 0)
            argc = 0;
    }
#else
    if (boot->cmdline_a == nullptr)
    {
        ErrorS("autodbg_run: missing command line\n");
        return -1;
    }
    {
        unsigned len = 0;
        while (boot->cmdline_a[len] != '\0')
            len++;

        char *buf = (char *)autodbg::common::alloc(len + 1);
        for (unsigned i = 0; i <= len; i++)
            buf[i] = boot->cmdline_a[i];

        argc = autodbg_split_cmdline_a(buf, (char **)s_argv, (int)(sizeof s_argv / sizeof s_argv[0]));
        if (argc < 0)
            argc = 0;
    }
#endif

    /* ---- 启动器选定的脚本覆盖 argv[1] ---- */
#ifdef _UNICODE
    if (boot->script_w != nullptr)
    {
        if (argc < 2)
        {
            s_argv[1] = (LPTSTR)boot->script_w;
            s_argv[2] = nullptr;
            argc      = 2;
        }
        else
        {
            s_argv[1] = (LPTSTR)boot->script_w;
        }
    }
#else
    if (boot->script_a != nullptr)
    {
        if (argc < 2)
        {
            s_argv[1] = (LPTSTR)boot->script_a;
            s_argv[2] = nullptr;
            argc      = 2;
        }
        else
        {
            s_argv[1] = (LPTSTR)boot->script_a;
        }
    }
#endif

    if (argc < 2 || s_argv[1] == nullptr)
    {
        ErrorS("autodbg_run: no script\n");
        return -1;
    }

    /* ---- 控制台 CP（与旧 mainCRTStartup 同口径） ---- */
#ifdef _UNICODE
    UINT old_cp = GetConsoleCP();
    if (old_cp != 65001)
        SetConsoleCP(65001);
#endif

    int r = run_once(argc, s_argv);

#ifdef _UNICODE
    if (old_cp != 65001)
        SetConsoleCP(old_cp);
#endif

    return r;
}

extern "C" void mainCRTStartup()
{
    int          argc;
    LPTSTR      *argv;
    LPTSTR      *envp;
    int          s;
    _CrtMemState before;
    InitCrtDbg(&before);
    if (__tgetmainargs(&argc, &argv, &envp, 0, &s) < 0)
    {
        CheckMemLeak(&before);
        ExitProcess(-1);
    }
    else
    {
#ifdef _UNICODE
        UINT old_cp = GetConsoleCP();
        if (old_cp != 65001)
            SetConsoleCP(65001);
#endif
        int r = run_once(argc, argv);
#ifdef _UNICODE
        if (old_cp != 65001)
            SetConsoleCP(old_cp);
#endif
        CheckMemLeak(&before);
        ExitProcess(r);
    }
}
