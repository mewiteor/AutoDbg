/* =============================================================================
 * src/core/util.cpp —— 核心通用工具实现（从 src/debugger/lib.c 1:1 移植，见 util.hpp）
 *
 * 移植原则：**逐字保留**旧行为（含既有缺陷，如 `ansi_to_utf8_n` 的 D12）；
 *   仅做两类机械替换：`_Generic` 宏→同名重载、`alloc_`/`free_`→common::alloc/release。
 *   差分测试：tests/unit/test_diff_lib.cpp（旧 lib.c ↔ 新 util.cpp 逐字节比对）。
 *
 * ❗例外（P7）：输出分流——控制台走 `WriteConsole`，非控制台走 `WriteFile`（见下面
 *   「输出」段与 `plan_p7_win9x.md` §2.4）。这是唯一一处偏离 1:1，差分测试需要例外。
 * ❗例外（S0，server 模式）：`PrintExN` 顶部多一个 **输出 sink**（`set_output_sink`），
 *   装了 sink 且它返回「已消费」时直接返回、不写句柄（`plan_server.md` §5.3）。
 *   默认（未装 sink）行为与 P7 完全一致。
 * ========================================================================== */
#include "util.hpp"

#include "common/dynapi.hpp" /* get_proc_as：Win95 缺失的 user32 API 动态解析 */

#include <intrin.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <strsafe.h>
#include <shlwapi.h>    /* PathFindFileNameA/W（log_msg 用） */
#include <tlhelp32.h>   /* Module32First/Next（非 Unicode 的 LjGetMappedFileNameA shim 用） */

namespace autodbg::core
{
/* ------------------------------ 输出 ------------------------------
 * 控制台走 `WriteConsole`（不受代码页影响，保证不乱码）；非控制台（文件/管道）走
 * `WriteFile`（重定向时才能抓到）。ANSI（legacy）变体一律 `WriteFile`：Win9x 的控制台
 * 是 ANSI-only、没有 Unicode 支持，按执行字符集（CP936）直接写字节。
 * 见 plan_p7_win9x.md §2.4。
 */
static HANDLE std_handle(DWORD o)
{
    HANDLE h = GetStdHandle(o);
    return (h == NULL || h == INVALID_HANDLE_VALUE) ? NULL : h;
}

static void write_bytes(HANDLE h, const char *s, size_t n)
{
    if (!h || n == 0)
        return;
    DWORD written = 0;
    WriteFile(h, s, (DWORD)n, &written, NULL);
}

/* ------------------- 输出 sink（server 模式，plan_server.md §5.3） -------------------
 * 两个转换助手都**不受 `_UNICODE` 影响**：ANSI（legacy）变体也要把宽/窄串统一成 UTF-8
 * 交给 sink，所以不重用本文件里只在 `_UNICODE` 下编译的 `utf16_to_utf8_n` /
 * `ansi_to_utf8_n`（后者还有 D12 缺陷：不写 NUL、不改长度）。 */
static output_sink_fn g_output_sink      = nullptr;
static void          *g_output_sink_ctx  = nullptr;

void set_output_sink(output_sink_fn fn, void *ctx)
{
    g_output_sink     = fn;
    g_output_sink_ctx = ctx;
}

bool output_sink_active()
{
    return g_output_sink != nullptr;
}

/* T1（D50）：子进程 std 句柄策略（详见 util.hpp 的注释） */
static bool g_child_stdio_capture = false;

void set_child_stdio_capture(bool enable)
{
    g_child_stdio_capture = enable;
}

bool child_stdio_capture()
{
    return g_child_stdio_capture;
}

/* STD_ERROR_HANDLE → 2，其余 → 1（与 plan_server.md §4.5 的 stdout/stderr 对应） */
static int output_stream_of(DWORD handle)
{
    return (handle == STD_ERROR_HANDLE) ? 2 : 1;
}

/* UTF-16 → UTF-8（不含 NUL；调用方负责 release） */
static LPSTR sink_utf16_to_utf8(LPCWSTR s, size_t n, size_t *out_len)
{
    *out_len = 0;
    if (s == nullptr || n == 0)
        return nullptr;
    int rn = WideCharToMultiByte(CP_UTF8, 0, s, (int)n, NULL, 0, NULL, NULL);
    if (rn <= 0)
        return nullptr;
    LPSTR r = (LPSTR)common::alloc((size_t)rn + 1);
    if (r == nullptr)
        return nullptr;
    WideCharToMultiByte(CP_UTF8, 0, s, (int)n, r, rn, NULL, NULL);
    r[rn]    = '\0';
    *out_len = (size_t)rn;
    return r;
}

/* CP_ACP 字节 → UTF-8（不含 NUL；调用方负责 release） */
static LPSTR sink_ansi_to_utf8(LPCSTR s, size_t n, size_t *out_len)
{
    *out_len = 0;
    if (s == nullptr || n == 0)
        return nullptr;
    int wn = MultiByteToWideChar(CP_ACP, 0, s, (int)n, NULL, 0);
    if (wn <= 0)
        return nullptr;
    LPWSTR w = (LPWSTR)common::alloc(sizeof(WCHAR) * (size_t)wn);
    if (w == nullptr)
        return nullptr;
    MultiByteToWideChar(CP_ACP, 0, s, (int)n, w, wn);
    LPSTR u = sink_utf16_to_utf8(w, (size_t)wn, out_len);
    common::release(w);
    return u;
}

void PrintExN(DWORD handle, const char *s, size_t n)
{
    /* sink 优先（且不依赖 std 句柄有效）：server 里通常没有控制台 */
    if (g_output_sink != nullptr && s != nullptr && n > 0)
    {
#ifdef _UNICODE
        /* UNICODE 构建：窄串由 /utf-8 保证就是 UTF-8，直接交出 */
        if (g_output_sink(g_output_sink_ctx, output_stream_of(handle), s, n))
            return;
#else
        /* ANSI（legacy）构建：执行字符集是 CP936 → 先转 UTF-8 */
        size_t un = 0;
        LPSTR  u  = sink_ansi_to_utf8(s, n, &un);
        if (u != nullptr)
        {
            bool consumed = g_output_sink(g_output_sink_ctx, output_stream_of(handle), u, un);
            common::release(u);
            if (consumed)
                return;
        }
#endif
    }

    HANDLE h = std_handle(handle);
    if (!h)
        return;

#ifdef _UNICODE
    DWORD mode;
    if (GetConsoleMode(h, &mode))
    {
        /* ❗控制台：窄串是 **UTF-8**（`/utf-8` 同时把执行字符集设成 UTF-8），
         * 而 `WriteConsoleA` 会按控制台的代码页（中文系统 = CP936）解释这些字节
         * → 中文全变乱码（实测：`server: 配置错误 …` 显示成 `閰嶇疆閿欒 …`）。
         * 所以先 UTF-8 → UTF-16，再用不受代码页影响的 `WriteConsoleW`。 */
        int    wn = (int)n;
        LPWSTR w  = utf8_to_utf16_n(s, &wn);
        if (w != nullptr && wn > 0)
        {
            WriteConsoleW(h, w, (DWORD)wn, NULL, NULL);
            common::release(w);
            return;
        }
        if (w != nullptr)
            common::release(w);
        /* 不是合法 UTF-8（少数来自 Win32 的 ANSI 串）→ 退回按字节写，至少不丢内容 */
        write_bytes(h, s, n);
        return;
    }
#endif
    write_bytes(h, s, n);
}

void PrintExN(DWORD handle, const wchar_t *s, size_t n)
{
    /* sink 优先（且不依赖 std 句柄有效） */
    if (g_output_sink != nullptr && s != nullptr && n > 0)
    {
        size_t un = 0;
        LPSTR  u  = sink_utf16_to_utf8(s, n, &un);
        if (u != nullptr)
        {
            bool consumed = g_output_sink(g_output_sink_ctx, output_stream_of(handle), u, un);
            common::release(u);
            if (consumed)
                return;
        }
    }

    HANDLE h = std_handle(handle);
    if (!h)
        return;

#ifdef _UNICODE
    DWORD mode;
    if (GetConsoleMode(h, &mode))
    {
        WriteConsoleW(h, s, (DWORD)n, NULL, NULL);
        return;
    }
    /* 重定向：UTF-8（无 BOM），接收方按 UTF-8 解码 */
    UINT cp = CP_UTF8;
#else
    /* ANSI（legacy）变体：Win9x 没有 Unicode 控制台，按 CP_ACP 转成 ANSI 字节写出去 */
    UINT cp = CP_ACP;
#endif

    int len = WideCharToMultiByte(cp, 0, s, (int)n, NULL, 0, NULL, NULL);
    if (len <= 0)
        return;
    LPSTR b = (LPSTR)common::alloc((size_t)len);
    if (!b)
        return;
    int w = WideCharToMultiByte(cp, 0, s, (int)n, b, len, NULL, NULL);
    write_bytes(h, b, (size_t)w);
    common::release(b);
}

void PrintExA(DWORD o, LPCSTR fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    PrintExVA(o, fmt, ap);
    va_end(ap);
}

#ifdef _UNICODE
void PrintExW(DWORD o, LPCWSTR fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    PrintExVW(o, fmt, ap);
    va_end(ap);
}
#endif

void PrintExVA(DWORD o, LPCSTR fmt, va_list ap)
{
    char buf[0x100];
    int  n = _vsnprintf(buf, 0x100, fmt, ap);
    if (n >= 0)
    {
        PrintExN(o, buf, n);
        return;
    }
    LPSTR s = NULL;
    for (unsigned int i = 2; i < 0x80000000; i++)
    {
        void *p = realloc(s, i);
        if (!p)
        {
            if (s) free(s);
            ErrorS("Out of Memory\n");
            return;
        }
        s = (LPSTR)p;
        n = _vsnprintf(s, i, fmt, ap);
        if (0 <= n)
        {
            PrintExN(o, s, n);
            free(s);
            return;
        }
    }
    ErrorS("i reached max\n");
}

#ifdef _UNICODE
void PrintExVW(DWORD o, LPCWSTR fmt, va_list ap)
{
    int n = _vscwprintf(fmt, ap);
    if (n <= 0) return;
    LPWSTR s = (LPWSTR)common::alloc((n + 1) * sizeof(WCHAR));
    if (SUCCEEDED(StringCchVPrintfW(s, n + 1, fmt, ap))) PrintEx(o, s);
    common::release(s);
}
#endif

void PrintF(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    PrintExVA(STD_OUTPUT_HANDLE, fmt, ap);
    va_end(ap);
}

#ifdef _UNICODE
void PrintF(const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    PrintExVW(STD_OUTPUT_HANDLE, fmt, ap);
    va_end(ap);
}
#endif

void ErrorF(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    PrintExVA(STD_ERROR_HANDLE, fmt, ap);
    va_end(ap);
}

#ifdef _UNICODE
void ErrorF(const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    PrintExVW(STD_ERROR_HANDLE, fmt, ap);
    va_end(ap);
}
#endif

void ErrorFV(const char *fmt, va_list ap)
{
    PrintExVA(STD_ERROR_HANDLE, fmt, ap);
}

#ifdef _UNICODE
void ErrorFV(const wchar_t *fmt, va_list ap)
{
    PrintExVW(STD_ERROR_HANDLE, fmt, ap);
}
#endif

void log_msg(const char *file, DWORD line, const char *fmt, ...)
{
    SYSTEMTIME lt;
    GetLocalTime(&lt);
    ErrorS("[");
    Error(PathFindFileNameA(file));
    ErrorF("][%04hu/%02hu/%02hu %02hu:%02hu:%02hu][%lu]", lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond, line);
    va_list ap;
    va_start(ap, fmt);
    ErrorFV(fmt, ap);
    va_end(ap);
    ErrorS("\n");
}

#ifdef _UNICODE
void log_msg(const wchar_t *file, DWORD line, const wchar_t *fmt, ...)
{
    SYSTEMTIME lt;
    GetLocalTime(&lt);
    ErrorS(L"[");
    Error(PathFindFileNameW(file));
    ErrorF(L"][%04hu/%02hu/%02hu %02hu:%02hu:%02hu][%lu]", lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond, line);
    va_list ap;
    va_start(ap, fmt);
    ErrorFV(fmt, ap);
    va_end(ap);
    ErrorS(L"\n");
}
#endif /* _UNICODE （宽字符 log_msg） */

void ErrorMessage(const char *file_name, DWORD line, const char *parent_function, const char *function, DWORD code, ...)
{
    ErrorF("　文件名：%s\n"
           "　行　号：%lu\n"
           "父函数名：%s\n"
           "　函数名：",
           file_name, line, parent_function);
    va_list ap;
    va_start(ap, code);
    ErrorFV(function, ap);
    va_end(ap);
    ErrorF("\n　错误码：%lu\n", code);
    LPSTR msg = NULL;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK, NULL, code,
                   MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&msg, 0, NULL);
    if (msg)
    {
        ErrorF("错误信息：%s\n", msg);
        LocalFree(msg);
    }
}

#ifdef _UNICODE
void ErrorMessage(const wchar_t *file_name, DWORD line, const wchar_t *parent_function, const wchar_t *function, DWORD code, ...)
{
    ErrorF(L"　文件名：%s\n"
           L"　行　号：%lu\n"
           L"父函数名：%s\n"
           L"　函数名：",
           file_name, line, parent_function);
    va_list ap;
    va_start(ap, code);
    ErrorFV(function, ap);
    va_end(ap);
    ErrorF(L"\n　错误码：%lu\n", code);
    LPWSTR msg = NULL;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK, NULL, code,
                   MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&msg, 0, NULL);
    if (msg)
    {
        ErrorF(L"错误信息：%s\n", msg);
        LocalFree(msg);
    }
}
#endif /* _UNICODE （宽字符 ErrorMessage） */

/* ------------------------------ CRC32 ------------------------------ */
static DWORD crc32_tab[256];

void init_tab()
{
    for (DWORD i = 0; i < 256; i++)
    {
        DWORD c = i;
        for (int j = 0; j < 8; j++) c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
        crc32_tab[i] = c;
    }
}

bool calc_crc32(LPCTSTR path, DWORD *crc32)
{
    if (!crc32) return false;
    DWORD  crc   = 0xFFFFFFFF;
    HANDLE hFile = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        E("CreateFile(\"%s\")", path);
        return false;
    }
    bool r = true;
#define BUF_SIZE 0x100
    for (;;)
    {
        DWORD sn;
        BYTE  buf[BUF_SIZE];
        if (!ReadFile(hFile, buf, BUF_SIZE, &sn, NULL))
        {
            E("ReadFile");
            r = false;
            break;
        }
        if (!sn) break;
        for (DWORD j = 0; j < sn; j++) crc = crc32_tab[(crc ^ buf[j]) & 0xFF] ^ (crc >> 8);
    }
    CloseHandle(hFile);
    *crc32 = crc ^ 0xFFFFFFFF;
    return r;
}

/* ------------------------- UTF / ANSI 转换 ------------------------- */
#ifdef _UNICODE
LPWSTR utf8_to_utf16(LPCSTR s, int *n)
{
    int t;
    if (!n) n = &t;
    *n = (int)strlen(s);
    return utf8_to_utf16_n(s, n);
}

LPSTR utf16_to_utf8(LPCWSTR s, int *n)
{
    int t;
    if (!n) n = &t;
    *n = (int)wcslen(s);
    return utf16_to_utf8_n(s, n);
}

LPWSTR utf8_to_utf16_n(LPCSTR s, int *n)
{
    int    rn = MultiByteToWideChar(CP_UTF8, 0, s, *n, NULL, 0);
    LPWSTR r  = (LPWSTR)common::alloc(sizeof(WCHAR) * (rn + 1));
    MultiByteToWideChar(CP_UTF8, 0, s, *n, r, rn);
    r[rn] = 0;
    *n    = rn;
    return r;
}

LPSTR utf16_to_utf8_n(LPCWSTR s, int *n)
{
    int   rn = WideCharToMultiByte(CP_UTF8, 0, s, *n, NULL, 0, NULL, NULL);
    LPSTR r  = (LPSTR)common::alloc(sizeof(CHAR) * (rn + 1));
    WideCharToMultiByte(CP_UTF8, 0, s, *n, r, rn, NULL, NULL);
    r[rn] = 0;
    *n    = rn;
    return r;
}

LPSTR ansi_to_utf8_n(LPCSTR s, int *n)
{
    int    rn = MultiByteToWideChar(CP_ACP, 0, s, *n, NULL, 0);
    LPWSTR r  = (LPWSTR)common::alloc(sizeof(WCHAR) * rn);
    MultiByteToWideChar(CP_ACP, 0, s, *n, r, rn);
    int   rn1 = WideCharToMultiByte(CP_UTF8, 0, r, rn, NULL, 0, NULL, NULL);
    LPSTR r1  = (LPSTR)common::alloc(sizeof(char) * rn1);
    WideCharToMultiByte(CP_UTF8, 0, r, rn, r1, rn1, NULL, NULL);
    common::release(r);
    return r1;
}
#endif

/* ---------------------- 老系统（Win95）API 等价包装 ----------------------
 * `GetAncestor` / `FlashWindowEx` 都是 Win98/NT4 才进 user32 的；Win95 上静态导入
 * 会让映射器在解析导入时硬失败。这里动态解析（dynapi 自带缓存），取不到就回落
 * 到 Win95 已有的等价 API。 */
HWND LjGetRootWindow(HWND hwnd)
{
    typedef HWND(WINAPI * GetAncestorT)(HWND, UINT);
    GetAncestorT pGetAncestor = common::get_proc_as<GetAncestorT>("user32.dll", "GetAncestor");
    if (pGetAncestor)
        return pGetAncestor(hwnd, GA_ROOT);
    /* Win95 回落：GA_ROOT = 沿父窗口一直走到顶（不含 owner 链） */
    HWND root = hwnd;
    for (HWND parent; (parent = GetParent(root)) != NULL;) root = parent;
    return root;
}

BOOL LjFlashWindow(HWND hwnd)
{
    typedef BOOL(WINAPI * FlashWindowExT)(PFLASHWINFO);
    FlashWindowExT pFlashWindowEx = common::get_proc_as<FlashWindowExT>("user32.dll", "FlashWindowEx");
    if (pFlashWindowEx)
    {
        FLASHWINFO fw;
        __stosb((PBYTE)&fw, 0, sizeof fw);
        fw.cbSize  = sizeof fw;
        fw.hwnd    = hwnd;
        fw.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG;
        fw.uCount  = 3;
        return pFlashWindowEx(&fw);
    }
    /* Win95 回落：只有 FlashWindow → 翻一次标题栏/任务栏（拿不到 uCount/TIMERNOFG 语义） */
    return FlashWindow(hwnd, TRUE);
}

/* ------------------------------ 字符串 ------------------------------ */
char *trim(char *s)
{
    while (0 < *s && *s <= ' ') s++;
    if (*s)
    {
        char *e = s + 1;
        while (*e) e++;
        do e--;
        while (0 < *e && *e <= ' ');
        e[1] = 0;
    }
    return s;
}

/* ------------------------ 非 Unicode 兼容 shim ------------------------ */
#ifndef _UNICODE
BOOL LjDebugSetProcessKillOnExit(BOOL KillOnExit) { return FALSE; }

HWND LjGetConsoleWindow() { return NULL; }

DWORD LjGetMappedFileNameA(DWORD dwPid, LPVOID lpv, LPSTR lpFilename, DWORD nSize)
{
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, dwPid);
    if (hSnap == INVALID_HANDLE_VALUE)
    {
        E("CreateToolhelp32Snapshot(%lu)", dwPid);
        return 0;
    }
    MODULEENTRY32 me = {sizeof(me)};
    if (Module32First(hSnap, &me))
    {
        do
        {
            if (me.modBaseAddr == (BYTE *)lpv)
            {
                StringCchCopyA(lpFilename, nSize, me.szExePath);
                break;
            }
        } while (Module32Next(hSnap, &me));
    }
    CloseHandle(hSnap);
    return strlen(lpFilename);
}
#endif
}

/* CRT 的浮点使用标记：MSVC 在任何用到浮点的 TU 里引用 `__fltused`，而本项目
 * `/nodefaultlib` 不链接 CRT 的那部分 → 必须由我们自己定义**一处**（不能两处）。
 * P3.B 模块 12：原先由旧的 lib.c（后来的 legacy_lib.c）定义；legacy_lib 现已移到
 * `tests/unit/`（只供差分测试、不进产物）→ 定义搬到这里。
 * ❗必须写成 C 链接（花括号形式）：C++ 的全局变量名会被修饰 → 链接器仍旧找不到 `__fltused`。 */
extern "C"
{
long _fltused;
}
