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
 * T1（D50）：`--json` / `--out=<path>`（plan_cli.md §0 D50）——本层是信封的组装点：
 *   ④ 读 `boot->json_mode` / `boot->json_out` → 跑脚本 → 组装并输出那一行 JSON；
 *   ⑤ JSON 模式下 `puts("done")` 被跳过（那一行必须独占 stdout），`print` 由 `output.hpp`
 *      收进缓冲（见 state.cpp）；`--out` 时 stdout 为空、内容改写文件。
 * T7 切片 D1：`boot->repl_mode != 0`（`--repl`，ABI 5）→ **不跑脚本**，走 `run_repl_mode()`：
 *   与脚本路径同一个 `create_state_with_libs()`（所以 `dbg`/`gui`/`open_file`/`help`/`dbglib`
 *   全都在），读 stdin 求值，返回码即退出码。
 * ========================================================================== */
#include <windows.h>

#include "core/core.hpp" /* DebuggerSession + set_current_session */
#include "core/util.hpp" /* E / ErrorS / Error / ErrorN / macro helpers */
#include "state.hpp"     /* run_script_file / g_wfilepath */
#include "repl.hpp"      /* T7 切片 D1：`--repl` 的主循环（run_repl） */
#include "file_io.hpp"    /* T4：set_client_read_timeout（等回包的上限） */
#include "client.hpp"    /* T3（D42）：`--remote` 客户端模式 */
#include "tcp_server.hpp" /* T5（D41）：server 只剩 raw TCP（HTTP 已删） */
#include "output.hpp"    /* T1（D50）：stdout/stderr 分桶 + `images` 登记表 */
#include "exec_guard.hpp" /* T1（D50）：`canceled`（guard_stop_reason）与 `detached`（guard_detached） */
#include "common/dynapi.hpp" /* get_proc_as：Win95 缺失的 kernel32 API 动态解析 */
#include "common/allocator.hpp"   /* T1（D50）：result 片段的释放 */
#include "common/json_writer.hpp" /* T1（D50）：信封的全转义出口（非 ASCII → \uXXXX） */
#include "common/selftest.hpp" /* S0：--selftest 纯函数自测（§11 验收） */

#include "../../common/cmdline.h"  /* autodbg_split_cmdline_a/w（P3） */
#include "../../sys/pack/boot.h"   /* autodbg_boot / AUTODBG_ABI_VERSION（P3） */

#include <assert.h>
#include <commdlg.h>
#include <intrin.h>
#include <shlwapi.h>
#include <string.h>      /* T1（D50）：strlen；T5：strcmp（脚本参数对齐） */
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
/* T5：脚本路径比对（自己写：影子头下 include <wchar.h> 会与 vcruntime_string.h 撞重定义） */
static bool path_same_a(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0')
    {
        char x = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
        char y = (*b >= 'A' && *b <= 'Z') ? (char)(*b - 'A' + 'a') : *b;
        if (x != y)
            return false;
        a++;
        b++;
    }
    return *a == *b;
}

static bool path_same_w(const wchar_t *a, const wchar_t *b)
{
    while (*a != L'\0' && *b != L'\0')
    {
        wchar_t x = (*a >= L'A' && *a <= L'Z') ? (wchar_t)(*a - L'A' + L'a') : *a;
        wchar_t y = (*b >= L'A' && *b <= L'Z') ? (wchar_t)(*b - L'A' + L'a') : *b;
        if (x != y)
            return false;
        a++;
        b++;
    }
    return *a == *b;
}
static autodbg::core::DebuggerSession *s_session = nullptr;

/* ---- T1（D50）：JSON 模式的进程级状态（一次进程只跑一次，不需要重置） ----
 * `s_json_mode` 被 `run_once` 读（跳 `puts("done")`），`s_json_result` 由 `run_script_file`
 * 的可选出参交回，`s_json_pid` 是**被调试进程**的 pid（未附加 = 0）。 */
static bool        s_json_mode   = false;
static const char *s_json_out    = nullptr;   /* `--out=<path>`（启动器给的 CP936 字节） */
static const char *s_json_result = nullptr;
static unsigned long s_json_pid  = 0;

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
    int r = run_script_file(argc, argv, s_json_mode ? &s_json_result : nullptr);

    /* T1（D50）：`pid` 取的是被调试进程（D43/D48：跨调用唯一的状态就是它，靠 pid 指认）。
     * 会话在 `run_once` 末尾被 delete，所以在这里取；未附加过就是 0。 */
    if (s_json_mode && s_session != nullptr)
        s_json_pid = s_session->proc().dwProcessId;

    return r;
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
 * finish_json_envelope —— T1（D50）：组装并输出机器可读结果（plan_cli.md §0 D50）
 *
 * 字段顺序就是契约：
 *   {"ok":bool,"exit":int,"canceled":bool,"detached":bool,"pid":int,
 *    "stdout":"...","stderr":"...","images":[...],"result":<脚本 return 值>}
 * 编码：一律 UTF-8 + 非 ASCII 走 `\uXXXX`（`json_writer::string_escaped` / `raw_escaped`），
 *   所以这一行是**纯 ASCII** —— legacy 变体（执行字符集 GBK）也能逐字节安全地写出去，
 *   不必改 `json_writer::string()` 的非 ASCII 原样输出口径。
 * 输出口径：`--out=<path>` → 二进制写文件（`\n` 不转 CRLF），内容 = 那一行 + 结尾 `\n`，
 *   stdout 留空；否则 stdout 只有那一行（`puts("done")` 与 `print` 的直接输出都被让位）。
 * 字段永远齐全：`images` 为空也输出 `[]`，`result` 编不了也输出 `null`（不省字段）。
 * `pid` = **被调试进程**（未附加 = 0）：与 `_autodbg_info().pid` / server JSON 里那个
 *   「AutoDbg 自己」的 pid 不是一个含义（两个含义不混用同一个字段名）。
 * `canceled` 走 `guard_stop_reason()`（本地 CLI 没有 guard 通道 → false）；`detached` 走
 *   `guard_detached()`（只有目标存活才算 true，D47）。
 * ========================================================================== */
static int finish_json_envelope(int exit_code)
{
    autodbg::host::output_snapshot snap = autodbg::host::output_end();

    autodbg::common::json_writer w;
    w.begin_object();
    w.key("ok");       w.boolean(exit_code == 0);
    w.key("exit");     w.number((long long)exit_code);
    w.key("canceled"); w.boolean(autodbg::host::guard_stop_reason() == autodbg::host::guard_cancel);
    w.key("detached"); w.boolean(autodbg::host::guard_detached() != 0);
    w.key("pid");      w.number((long long)s_json_pid);
    w.key("stdout");   w.string_escaped(snap.out, snap.out_len);
    w.key("stderr");   w.string_escaped(snap.err, snap.err_len);
    w.key("images");
    w.begin_array();
    for (size_t i = 0; i < autodbg::host::output_path_count(); i++)
    {
        const char *p = autodbg::host::output_path_at(i);
        if (p != nullptr)
            w.string_escaped(p, strlen(p));
    }
    w.end_array();
    w.key("result");
    w.raw_escaped(s_json_result, (s_json_result != nullptr) ? strlen(s_json_result) : 0);
    w.end_object();

    if (w.data() == nullptr)
        return exit_code;   /* 缓冲分配失败：不输出半行 */

    size_t n = w.size();

    if (s_json_out != nullptr)
    {
        /* `--out`：内容 = 那一行 + 结尾 `\n`，stdout 留空。路径是启动器给的 ACP（CP936）字节。 */
        HANDLE h;
#ifdef _UNICODE
        wchar_t wpath[260];
        int wn = MultiByteToWideChar(CP_ACP, 0, s_json_out, -1, wpath, (int)(sizeof wpath / sizeof wpath[0]));
        if (wn <= 0)
        {
            ErrorS("autodbg_run: cannot convert --out path\n");
            return exit_code;
        }
        h = CreateFileW(wpath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
#else
        h = CreateFileA(s_json_out, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
#endif
        if (h == INVALID_HANDLE_VALUE)
        {
            ErrorS("autodbg_run: cannot write --out file\n");
            return exit_code;
        }

        DWORD written = 0;
        BOOL  okw     = WriteFile(h, w.data(), (DWORD)n, &written, NULL);
        BOOL  oke     = WriteFile(h, "\n", 1, &written, NULL);
        CloseHandle(h);
        if (!okw || !oke)
            ErrorS("autodbg_run: short write to --out file\n");
        return exit_code;
    }

    /* 无 `--out`：那一行就是 stdout 的全部内容（此时 sink 已被 output_end 卸掉） */
    autodbg::core::PrintN(w.data(), n);
    autodbg::core::PrintS("\n");

    if (s_json_result != nullptr)
        autodbg::common::release((void *)s_json_result);

    return exit_code;
}

/* =============================================================================
 * run_once —— 宿主无关的「跑一次」（P3 新提出来，原为 mainCRTStartup 内联体）
 *
 * 顺序与旧 `mainCRTStartup` 逐字一致：isa 初始化 → init_tab → _tmain → 释放会话 → puts("done")。
 * 不含 `ExitProcess`、不含控制台 CP 切换（那两件事归调用方：exe 入口做 ExitProcess，
 * DLL 的 `autodbg_run` 把返回值交给启动器）。
 * T1（D50）：JSON 模式时 `puts("done")` 被跳过（那一行 JSON 必须独占 stdout），改为
 *   组装信封；非 JSON 路径逐字节不变。
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

    if (s_json_mode)
        return finish_json_envelope(r);   /* T1（D50）：stdout 只有一行（或写 --out 文件） */

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
 * run_repl_mode —— T7 切片 D1：`--repl`（ABI 5 的 `repl_mode`）
 *
 * 与 `run_once` **同口径的前半段**（isa 初始化 → init_tab → 会话挂载），然后不跑脚本：
 *   建状态（与脚本路径同一个 `create_state_with_libs()`，D1 要求「复用同一个 state」）
 *   → `run_repl` 读 stdin 求值 → `lua_close` → 会话收尾。返回码即退出码（不 `exit()`）。
 * ❗为什么单独一个函数而不是塞进 `run_once`：`run_once` 的 `_tmain` 先要一个脚本文件
 *   （`g_wfilepath`，否则弹文件对话框），而 REPL 没有脚本；`puts("done")` 对 REPL 也只是噪音。
 * ========================================================================== */
static int run_repl_mode()
{
#ifndef _WIN64
    __isa_available_init();
#endif
    init_tab();

    /* 会话与脚本路径同口径：旧契约名（`Run`/`Attach`）走 `current_session()`。 */
    s_session = new autodbg::core::DebuggerSession();
    autodbg::core::set_current_session(s_session);

    lua_State *L = create_state_with_libs();
    int        r = -1;
    if (L == nullptr)
        ErrorS("repl: cannot create lua state\n");
    else
    {
        r = autodbg::host::run_repl(L);
        lua_close(L);
    }

    delete s_session;
    s_session = nullptr;
    autodbg::core::set_current_session(nullptr);
    return r;
}

/* =============================================================================
 * autodbg_run —— 模块 DLL 的**唯一导出**（Plan §2.3 / P3）
 *
 * 等价于旧 `mainCRTStartup` 去掉 `__getmainargs` 与 `ExitProcess`：
 *   ① 校验 `boot->abi` / `boot->size`（不匹配就拒绝，而不去读半个结构）；
 *   ② 从 `boot->cmdline_a/w` 自己拆 argv（映射后的模块拿不到进程参数）；
 *   ③ `boot->script_a/w` 非空则覆盖 `argv[1]`（启动器已经确定好脚本了）；
 *   ④ 调 `run_once`，返回值即退出码（由启动器负责 ExitProcess）。
 * T1（D50）：`boot->json_mode` / `boot->json_out` → 跑脚本前装输出收集器（不 echo），
 *   并把被调试进程的 std 句柄改成 stderr 副本（`set_child_stdio_capture`，命 stdout 只留那一行），
 *   跑完后由 `finish_json_envelope` 输出那一行（或写 `--out` 文件）。退出码语义不变。
 * ========================================================================== */
extern "C" __declspec(dllexport) int __cdecl autodbg_run(const autodbg_boot *boot)
{
    if (boot == nullptr || boot->abi != AUTODBG_ABI_VERSION || boot->size != sizeof(autodbg_boot))
    {
        ErrorS("autodbg_run: boot ABI mismatch\n");
        return -1;
    }

    /* ---- S0（plan_server.md D27）：这两个模式都不需要脚本 → 在拆命令行之前分流 ---- */
    /* T4（D45）：一次文件操作/等回包的上限来自 ABI 4 的 --read-timeout；
     * ❗必须在 server / client 分流**之前**设：server 自己也要用它等客户端回包。 */
    autodbg::host::set_client_read_timeout(boot->read_timeout_ms);

    if (boot->selftest != 0)
        return run_selftest_mode();
    if (boot->server_mode != 0)
        return autodbg::server::tcp_server_main(boot);
    /* ---- T7 切片 D1：`--repl`（与上面两个同类：**不需要脚本**，也不碰命令行） ----
     * `--repl` + `--server` 的形态属切片 D2（启动器现在就拒绝），所以这里不接 server。 */
    if (boot->repl_mode != 0)
        return run_repl_mode();

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

    /* ---- 启动器选定的脚本覆盖 argv[1] ----
     * ❗T5：启动器传来的是**整条命令行**（含它自己的选项），所以直接覆盖 argv[1] 会把
     *   「脚本路径」留在 argv[2] 里 → 脚本的 `...` 第一个参数变成脚本路径
     *   （实测：`--quiet a.lua 123` 里 `...` == `a.lua`）。这里先找到启动器那一份脚本在
     *   原 argv 里的位置，再把**它后面的**元素搬到 argv[2..]。 */
    {
#ifdef _UNICODE
        const wchar_t *wanted = boot->script_w;
#else
        const char *wanted = boot->script_a;
#endif
        if (wanted != nullptr)
        {
            int script_at = -1;
            for (int i = 0; i < argc; i++)
            {
                /* ❗不能比指针：启动器传的是它自己静态缓冲里的那份，本层 argv 是重新拆
                 * 出来的拷贝。比**字符串**（两边都来自同一条命令行文本，方向一致）。 */
#ifdef _UNICODE
                bool same = (s_argv[i] != nullptr && boot->script_w != nullptr &&
                             path_same_w(s_argv[i], boot->script_w));
#else
                bool same = (s_argv[i] != nullptr && boot->script_a != nullptr &&
                             path_same_a(s_argv[i], boot->script_a));
#endif
                if (same)
                {
                    script_at = i;
                    break;
                }
            }

            if (script_at >= 0)
            {
                int j = 0;
                s_argv[1] = s_argv[script_at];
                j         = 2;
                for (int i = script_at + 1; i < argc; i++)
                    s_argv[j++] = s_argv[i];
                s_argv[j] = nullptr;
                argc      = j;
            }
            else
            {
                /* 指针对不上（例如启动器改写过命令行）：退化成旧行为（至少有脚本） */
                if (argc < 2)
                {
                    s_argv[1] = (LPTSTR)((boot->script_a != nullptr) ? (LPTSTR)boot->script_a : (LPTSTR)boot->script_w);
                    s_argv[2] = nullptr;
                    argc      = 2;
                }
            }
        }
    }

    /* ---- T3（D42）：客户端模式：不跑本地脚本，连 server 把脚本/控制命令发过去。
     * ❗必须在**装输出收集器之前**分派：客户端的 stdout/stderr 是它自己控制的
     *   （非 JSON 时直通、JSON 时自己组一行信封），收集器那是给本地脚本用的。 */
    if (boot->remote_host != nullptr)
        return autodbg::host::client_main(boot);

    /* ---- T1（D50）：JSON 模式 —— 在跑脚本**之前**装收集器（不 echo）：
     *      `print` / `Error*` / `log_msg` 都只进缓冲，stdout 才能被那一行 JSON 独占；
     *      `--quiet` + `--json` 时 stdout 仍然只有那一行。`--out` 单独给也算 JSON 模式。 */
    s_json_mode = (boot->json_mode != 0 || boot->json_out != nullptr);
    s_json_out  = boot->json_out;
    if (s_json_mode)
    {
        autodbg::host::output_begin(0, false);
        /* T1（D50）：被调试进程的 stdout/stderr 不能落到我们的 stdout 上（否则 `cmd /c ping`
         * 这类目标的输出会顶在那一行 JSON 前面）。core 不认识 json ⇒ 开关放宿主层。
         * 目标是把它指到我们 stderr 的可继承副本（见 `core/util.hpp` 的注释）。 */
        autodbg::core::set_child_stdio_capture(true);
    }

    if (argc < 2 || s_argv[1] == nullptr)
    {
        ErrorS("autodbg_run: no script\n");
        /* T1（D50）：字段必须齐全 —— 没有脚本也要给出那一行（ok=false、exit=-1） */
        if (s_json_mode)
            return finish_json_envelope(-1);
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
