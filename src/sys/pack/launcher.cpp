/* =============================================================================
 * src/sys/pack/launcher.cpp —— 启动器主体（Plan §2.3 / §2.5 / §2.6 / D5 / D7 / D9 / D10）
 *
 * 职责：解析选项 → 定脚本（argv 或文件对话框）→ 定 flavor（CLI > 脚本标记 > 默认 lua）
 *       → 定 variant（自动探测，`--variant` 可强制）→ 解压载荷 → 映射模块 →
 *       组装 `autodbg_boot` → 调 `autodbg_run` → 用返回值 ExitProcess。
 * 来源：新写（Plan P3 起）。原型 `tools/PackEXE` 的 loader 只会「按名字解一个 DLL」。
 * 与旧实现的差异：
 *   ① flavor 选择：`--flavor` > 脚本首行标记（`--!lua` / `--!luajit`）> 字节码 magic > 默认 lua；
 *      未知标记**报错退出**（D9），不静默降级；
 *   ② variant 回退链（F12）：先按平台探测选，`map()` 失败则换另一个变体重试，并打印原因；
 *   ③ 无参时的文件对话框（D7）：优先 `GetOpenFileNameW`（动态取，9x 无 W 版则降级 A 版）。
 * T1（D50）：解析 `--json` / `--out=<path>` → 填进 `autodbg_boot`（ABI 3 的两个追加字段），
 *   信封本身在模块里组装（`main.cpp`）。`--out` 单独给也算 JSON 模式；与 `--server` 互斥
 *   （同时给 → 报错、退出码 2）；JSON 模式不弹文件对话框，启动器自己的诊断走 stderr
 *   （stdout 必须被那一行 JSON 独占）。
 * T7 切片 D1：`--repl` → `boot.repl_mode = 1`（ABI 5），**不需要脚本**。与 `--json` / `--out`
 *   互斥（那一行必须独占 stdout）、与 `--server` 互斥（REPL 跑在 server 上属切片 D2，本切片
 *   只报错），两者都退 2。REPL 主循环在模块里（`scripting/lua/repl.cpp`）。
 *
 * ❗启动器是 **ANSI 编译**（要能在 Win9x 上跑）：所有 API 显式带 A/W 后缀；
 *   `GetCommandLineW` / `GetOpenFileNameW` 一律走 `GetProcAddress`（9x 的 kernel32/comdlg32 没有 W 版）。
 * ========================================================================== */
#include <windows.h>
#include <stddef.h>
#include <string.h>   /* memset（影子头） */

#include "../../common/allocator.hpp"
#include "../../common/cmdline.h"
#include "../version.h" /* AUTODBG_VERSION（构建期生成，见 make.bat :genversion） */
#include "boot.h"
#include "error.hpp"
#include "module_loader.hpp"
#include "payload.hpp"

using autodbg::sys::pack::load_error;
using autodbg::sys::pack::mapped_module;
using autodbg::sys::pack::payload;
using autodbg::sys::pack::stage;

extern "C" const unsigned char g_packdata[];
extern "C" const unsigned int   g_packdata_size;

namespace
{
constexpr unsigned k_flavor_lua     = 0;
constexpr unsigned k_flavor_luajit  = 1;
constexpr unsigned k_variant_modern = 0;
constexpr unsigned k_variant_legacy = 1;

/* ---- 输出（WriteFile 而不是 WriteConsole：重定向时也要能抓到） ---- */
void write_handle(DWORD which, const char *s)
{
    HANDLE   h       = GetStdHandle(which);
    DWORD    written = 0;
    unsigned len     = 0;

    if (h == NULL || h == INVALID_HANDLE_VALUE)
        return;
    while (s[len] != '\0')
        len++;
    WriteFile(h, s, len, &written, NULL);
}

void out(const char *s) { write_handle(STD_OUTPUT_HANDLE, s); }
void out_err(const char *s) { write_handle(STD_ERROR_HANDLE, s); }

void out_u32(unsigned v)
{
    char tmp[16];
    char buf[16];
    int  i = 0;
    int  n = 0;

    do
    {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v != 0);
    while (i > 0)
        buf[n++] = tmp[--i];
    buf[n] = '\0';
    out(buf);
}

/* 错误流的数字（`report()` 的字段必须与它同流，否则重定向后顺序/内容都会错） */
void out_u32_err(unsigned v)
{
    char tmp[16];
    char buf[16];
    int  i = 0;
    int  n = 0;

    do
    {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v != 0);
    while (i > 0)
        buf[n++] = tmp[--i];
    buf[n] = '\0';
    out_err(buf);
}

/* ---- 字符串小工具（零 CRT：不用 strcpy/strcat） ---- */
void append(char *dst, unsigned cap, const char *src)
{
    unsigned n = 0;
    while (dst[n] != '\0')
        n++;
    while (*src != '\0' && n + 1 < cap)
        dst[n++] = *src++;
    dst[n] = '\0';
}

void lower_copy(char *dst, unsigned cap, const char *src, unsigned src_len)
{
    unsigned n = 0;
    for (; n + 1 < cap && n < src_len && src[n] != '\0'; n++)
    {
        char c = src[n];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c + 32);
        dst[n] = c;
    }
    dst[n] = '\0';
}

bool str_eq(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b)
    {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

bool str_ieq(const char *a, const char *b)
{
    for (;; a++, b++)
    {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb + 32);
        if (ca != cb)
            return false;
        if (ca == '\0')
            return true;
    }
}

bool starts_with(const char *s, const char *prefix)
{
    while (*prefix != '\0')
    {
        if (*s++ != *prefix++)
            return false;
    }
    return true;
}

const char *arch_suffix()
{
#ifdef _WIN64
    return "64";
#else
    return "86";
#endif
}

/* flavor: 0 = lua(5.5)，1 = luajit；variant: 0 = modern，1 = legacy（§2.1 的命名） */
void build_module_name(char *dst, unsigned cap, unsigned flavor, unsigned variant)
{
    dst[0] = '\0';
    append(dst, cap, variant == k_variant_modern ? "autodbg_" : "autodbglegacy_");
    append(dst, cap, flavor == k_flavor_lua ? "lua" : "luajit");
    append(dst, cap, "_");
    append(dst, cap, arch_suffix());
    append(dst, cap, ".dll");
}

const char *stage_name(stage s)
{
    switch (s)
    {
    case stage::payload:   return "payload";
    case stage::header:    return "header";
    case stage::section:   return "section";
    case stage::reloc:     return "reloc";
    case stage::import:    return "import";
    case stage::exception: return "exception";
    case stage::entry:     return "entry";
    default:               return "none";
    }
}

void report(const char *what, const load_error &e)
{
    out_err(what);
    out_err(": stage=");
    out_err(stage_name(e.where));
    out_err(" detail=");
    out_err(e.detail != nullptr ? e.detail : "(null)");
    out_err(" code=");
    out_u32_err(e.code);
    out_err(" line=");
    out_u32_err(e.line);
    out_err("\n");
}

/* ---- 平台探测（§2.6）：9x / NT4 走 legacy ---- */
unsigned detect_variant()
{
    OSVERSIONINFOA vi;

    memset(&vi, 0, sizeof vi);
    vi.dwOSVersionInfoSize = sizeof vi;

    /* GetVersionExA 在新 SDK 里被标为已废弃（仅因 manifest 机制），这里确实需要它：
     * 要区分 Win9x（VER_PLATFORM_WIN32_WINDOWS），而新 API 在 9x 上不存在。 */
#pragma warning(push)
#pragma warning(disable : 4996)
    if (GetVersionExA(&vi))
    {
        if (vi.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS)   /* Win95/98/ME */
            return k_variant_legacy;
        if (vi.dwMajorVersion < 5)                            /* NT4 及更早 */
            return k_variant_legacy;
    }
#pragma warning(pop)
    return k_variant_modern;
}

/* `GetCommandLineW` 在 Win9x 的 kernel32 里不存在 → 动态取（否则 9x 连启动器都打不开） */
const wchar_t *query_cmdline_w()
{
    typedef const wchar_t *(__stdcall * get_cmdline_w_fn)();

    HMODULE          k32 = GetModuleHandleA("kernel32.dll");
    get_cmdline_w_fn fn  = nullptr;

    if (k32 == NULL)
        return nullptr;
    fn = (get_cmdline_w_fn)GetProcAddress(k32, "GetCommandLineW");
    return (fn != nullptr) ? fn() : nullptr;
}

/* ---- 脚本路径：A/W 两份同时维护（模块是 UNICODE 构建时只看 W） ---- */
char    g_script_a[2048];
wchar_t g_script_w[1024];

void set_script_ansi(const char *path)
{
    unsigned n = 0;
    while (path[n] != '\0' && n + 1 < sizeof g_script_a)
    {
        g_script_a[n] = path[n];
        n++;
    }
    g_script_a[n] = '\0';

    g_script_w[0] = 0;
    MultiByteToWideChar(CP_ACP, 0, g_script_a, -1, g_script_w,
                        (int)(sizeof g_script_w / sizeof g_script_w[0]));
}

void set_script_wide(const wchar_t *path)
{
    unsigned cap = (unsigned)(sizeof g_script_w / sizeof g_script_w[0]);
    unsigned n   = 0;

    while (path[n] != 0 && n + 1 < cap)
    {
        g_script_w[n] = path[n];
        n++;
    }
    g_script_w[n] = 0;

    g_script_a[0] = '\0';
    WideCharToMultiByte(CP_ACP, 0, g_script_w, -1, g_script_a, (int)sizeof g_script_a, NULL, NULL);
}

/* ---- 无参时的文件对话框（D7）：优先 W，9x 降级 A ---- */
bool ask_script()
{
    typedef BOOL(__stdcall * get_open_file_name_w_fn)(OPENFILENAMEW *);

    HMODULE                 cd = GetModuleHandleA("comdlg32.dll");
    get_open_file_name_w_fn fw = nullptr;

    if (cd != NULL)
        fw = (get_open_file_name_w_fn)GetProcAddress(cd, "GetOpenFileNameW");

    if (fw != nullptr)
    {
        wchar_t buf[MAX_PATH];
        buf[0] = 0;

        OPENFILENAMEW ofn;
        memset(&ofn, 0, sizeof ofn);
        ofn.lStructSize = sizeof ofn;
        ofn.lpstrFilter = L"lua script\0*.lua\0All files\0*.*\0";
        ofn.lpstrFile   = buf;
        ofn.nMaxFile    = MAX_PATH;
        ofn.lpstrTitle  = L"AutoDbg: open lua script";
        ofn.lpstrDefExt = L".lua";
        ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NONETWORKBUTTON;

        if (!fw(&ofn))
            return false;
        set_script_wide(buf);
        return true;
    }

    /* 9x：只有 A 版 */
    char buf[MAX_PATH];
    buf[0] = 0;

    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFilter = "lua script\0*.lua\0All files\0*.*\0";
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrTitle  = "AutoDbg: open lua script";
    ofn.lpstrDefExt = ".lua";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NONETWORKBUTTON;

    if (!GetOpenFileNameA(&ofn))
        return false;
    set_script_ansi(buf);
    return true;
}

/* ---- flavor 选择（§2.5 / D9 / D10）----
 * 只看**最开始若干行**（跳过空行）：UTF-8 BOM 跳过；UTF-16 BOM 视为「无标记」。
 * `--!autodbg` / `--!adbg` 是**编辑器侧标记**（远程地址 / exe / arch），不是 flavor
 *   → 跳过整行继续看下一行（这样同一份脚本既能被 nvim 插件跑，也能直接命令行跑）。
 * 返回：0/1 = 选定；2 = 无标记；3 = 未知标记（调用方报错退出）。 */
unsigned flavor_from_script(const char *path, char *bad_name, unsigned bad_cap)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 2;   /* 打不开 → 静默走默认（错误由模块报，保持「文件不存在」的文案与退出码） */

    unsigned char buf[256];
    DWORD         read = 0;
    bool          ok   = (ReadFile(h, buf, sizeof buf, &read, NULL) != 0);
    CloseHandle(h);
    if (!ok || read == 0)
        return 2;

    /* UTF-16 BOM：不解析（标记规范只定义 UTF-8 文本） */
    if (read >= 2 && ((buf[0] == 0xFF && buf[1] == 0xFE) || (buf[0] == 0xFE && buf[1] == 0xFF)))
        return 2;

    unsigned i = 0;
    if (read >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF)
        i = 3;   /* UTF-8 BOM */

    /* 字节码 magic 嗅探：\x1bLua + 版本字节 / \x1bLJ */
    if (i < read && buf[i] == 0x1B)
    {
        if (i + 3 < read && buf[i + 1] == 'L' && buf[i + 2] == 'J')
            return k_flavor_luajit;
        if (i + 4 < read && buf[i + 1] == 'L' && buf[i + 2] == 'u' && buf[i + 3] == 'a')
            return k_flavor_lua;
        return 2;
    }

    for (;;)
    {
        /* 跳空白 */
        while (i < read && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n'))
            i++;

        if (i + 3 > read || buf[i] != '-' || buf[i + 1] != '-' || buf[i + 2] != '!')
            return 2;   /* 第一个非空行不是标记 → 无标记 */

        i += 3;
        char     name[16];
        unsigned n = 0;
        while (i < read && n + 1 < sizeof name)
        {
            unsigned char c = buf[i];
            if (c == '\r' || c == '\n' || c == ' ' || c == '\t')
                break;
            name[n++] = (char)c;
            i++;
        }
        name[n] = '\0';

        if (str_ieq(name, "lua") || str_ieq(name, "lua55") || str_ieq(name, "lua5.5"))
            return k_flavor_lua;
        if (str_ieq(name, "luajit") || str_ieq(name, "jit"))
            return k_flavor_luajit;

        /* `--!autodbg ...`（nvim 插件的脚本头标记）不是 flavor → 跳过整行继续扫 */
        if (str_ieq(name, "autodbg") || str_ieq(name, "adbg"))
        {
            while (i < read && buf[i] != '\n')
                i++;
            continue;
        }

        /* 未知标记：报错退出（D9，让 typo 可见） */
        unsigned k = 0;
        for (; name[k] != '\0' && k + 1 < bad_cap; k++)
            bad_name[k] = name[k];
        bad_name[k] = '\0';
        return 3;
    }
}

/* 从磁盘读一个模块（D5 的 `--module-dir`，仍走内存映射，不 LoadLibrary） */
unsigned char *read_file(const char *path, unsigned *size)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return nullptr;

    DWORD hi = 0;
    DWORD lo = GetFileSize(h, &hi);
    if (hi != 0 || lo == 0)
    {
        CloseHandle(h);
        return nullptr;
    }

    unsigned char *buf = (unsigned char *)autodbg::common::alloc(lo);
    DWORD          read = 0;
    if (!ReadFile(h, buf, lo, &read, NULL) || read != lo)
    {
        CloseHandle(h);
        autodbg::common::release(buf);
        return nullptr;
    }
    CloseHandle(h);
    *size = lo;
    return buf;
}

/* ===== S0（plan_server.md §4.1）：`--server*` 选项的小工具 ===== */

/* 把无符号数追加到 dst（零 CRT：不用 sprintf） */
void append_u32(char *dst, unsigned cap, unsigned v)
{
    char tmp[16];
    int  i = 0;
    int  n = 0;

    do
    {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v != 0);
    while (dst[n] != '\0')
        n++;
    while (i > 0 && (unsigned)n + 1 < cap)
        dst[n++] = tmp[--i];
    dst[n] = '\0';
}

/* 解析十进制无符号（只允许数字；超界/空串失败） */
bool parse_u32(const char *s, unsigned *out, unsigned min_v, unsigned max_v)
{
    unsigned long long v = 0;

    if (s == nullptr || *s == '\0')
        return false;
    for (const char *p = s; *p != '\0'; p++)
    {
        if (*p < '0' || *p > '9')
            return false;
        v = v * 10 + (unsigned)(*p - '0');
        if (v > 0xFFFFFFFFull)
            return false;
    }
    if (v < min_v || v > max_v)
        return false;
    *out = (unsigned)v;
    return true;
}

/* 把 addr 里的端口换成 port（addr 为空 → 127.0.0.1:port） */
void build_addr(char *dst, unsigned cap, const char *addr, unsigned port)
{
    char     host[64];
    unsigned n = 0;

    host[0] = '\0';
    if (addr != nullptr)
    {
        while (addr[n] != '\0' && addr[n] != ':' && n + 1 < sizeof host)
        {
            host[n] = addr[n];
            n++;
        }
    }
    host[n] = '\0';
    if (host[0] == '\0')
    {
        const char *def = "127.0.0.1";
        n = 0;
        while (def[n] != '\0' && n + 1 < sizeof host)
        {
            host[n] = def[n];
            n++;
        }
        host[n] = '\0';
    }

    dst[0] = '\0';
    append(dst, cap, host);
    append(dst, cap, ":");
    append_u32(dst, cap, port);
}

/* T3（D42）：「host:port」拆开。`port` 缺省为 0（由调用方代以 8642）。
 * 只有 IPv4 文本 / `localhost`（不支持 IPv6 — 与 `net_parse_ipv4` 一致）。 */
bool split_host_port(const char *s, char *host, unsigned cap, unsigned *port)
{
    if (s == nullptr || host == nullptr || port == nullptr || cap == 0)
        return false;
    unsigned n = 0;
    while (s[n] != '\0' && s[n] != ':')
    {
        if (n + 1 >= cap)
            return false;   /* host 太长 */
        host[n] = s[n];
        n++;
    }
    host[n] = '\0';
    if (n == 0)
        return false;

    *port = 0;
    if (s[n] != ':')
        return true;        /* 只有 host：端口用缺省 */
    n++;
    if (s[n] == '\0')
        return false;       /* 冒号后面什么都没有 */
    unsigned v = 0;
    while (s[n] != '\0')
    {
        if (s[n] < '0' || s[n] > '9')
            return false;
        v = v * 10u + (unsigned)(s[n] - '0');
        if (v > 65535u)
            return false;
        n++;
    }
    *port = v;
    return true;
}

/* 十进制无符号解析（`--pid=` / `--read-timeout=` / `--max-frame=` 用）；失败返回 false */
bool parse_udec(const char *s, unsigned *out)
{
    if (s == nullptr || out == nullptr || s[0] == '\0')
        return false;
    unsigned v = 0;
    for (unsigned i = 0; s[i] != '\0'; i++)
    {
        if (s[i] < '0' || s[i] > '9')
            return false;
        unsigned d = (unsigned)(s[i] - '0');
        if (v > (0xFFFFFFFFu - d) / 10u)
            return false;   /* 溢出 */
        v = v * 10u + d;
    }
    *out = v;
    return true;
}
}

/* ---- `--version` / `--help` ----
 * 输出一律 **ASCII**：launcher 是 ANSI 编译、走 `WriteFile` 写字节，
 * 中文在 CP936 控制台上会乱码（模块那边走 `WriteConsoleW` 才不受影响）。 */
void print_version()
{
    out("AutoDbg ");
    out(AUTODBG_VERSION);
    out(" (");
    out(AUTODBG_VERSION_SOURCE);
#if AUTODBG_VERSION_DIRTY
    out(", dirty");
#endif
    out(", abi ");
    out_u32((unsigned)AUTODBG_ABI_VERSION);
    out(", ");
    out(AUTODBG_VERSION_ARCH);
    out(")\n");
}

void print_usage()
{
    out("AutoDbg ");
    out(AUTODBG_VERSION);
    out(" (");
    out(AUTODBG_VERSION_ARCH);
    out(", abi ");
    out_u32((unsigned)AUTODBG_ABI_VERSION);
    out(")\n\n");

    out("usage: AutoDbg_<arch>.exe [options] [<script.lua>] [script args...]\n\n");
    out("options:\n");
    out("  -h, --help                 show this help and exit\n");
    out("      --version              show version / abi / arch and exit\n");
    out("      --flavor=lua|luajit    interpreter (default: lua 5.5)\n");
    out("      --variant=modern|legacy  force variant (debugging only)\n");
    out("      --list                 list modules embedded in the payload\n");
    out("      --module-dir=<dir>     load module DLLs from <dir> (still memory-mapped)\n");
    out("      --quiet                do not show error message boxes\n");
    out("      --selftest             run built-in pure-function self test\n");
    out("      --repl                 read Lua statements from stdin (piped or console)\n");
    out("      --json                 print one machine-readable JSON line (D50)\n");
    out("      --out=<path>           write the same JSON to <path>; stdout stays empty\n");
    out("      --server[=tcp]           server mode (raw tcp frames; http was removed, D41)\n");
    out("      --server-addr=<ip:port>  listen address (default 127.0.0.1:8642)\n");
    out("      --server-port=<port>     only override the port\n");
    out("      --server-token=<hex|auto|none>\n");
    out("      --server-max-body=<bytes>  request body limit (default 8388608)\n");
    out("      --server-queue=<n>       queue depth (default 8)\n");
    out("      --server-timeout=<sec>   per-request timeout (0 = none)\n");
    out("      --server-log=<path>      diagnostic log file\n");
    out("      --server-no-echo         do not echo executed output to console\n");
    out("      (--json/--out are mutually exclusive with --server, exit code 2)\n");
    out("      (--repl is mutually exclusive with --json/--out, and with --server until D2)\n\n");
    out("client (raw tcp, D42):\n");
    out("      --remote=<host:port>   run the script on a remote server (default port 8642)\n");
    out("      --remote-token=<hex>    token for the handshake (loopback needs none)\n");
    out("      --pid=<n>               explicit target pid sent to the server\n");
    out("      --read-timeout=<ms>     per-file-operation timeout (default 5000)\n");
    out("      --max-frame=<bytes>     largest frame the client accepts (default 8388608)\n");
    out("      --cancel | --kill | --detach   control the running execution\n");
    out("      --health | --version            query server info over the tcp connection\n\n");
    out("script head markers:\n");
    out("  --!lua / --!luajit         select interpreter (CLI --flavor wins)\n");
    out("  --!autodbg ...             editor-side marker (ignored, line is skipped)\n\n");
    out("exit codes: 0 ok / -1 load or run failed / lua error code / 2 launcher error\n");
}

extern "C" int launcher_main(int argc, char **argv)
{
    unsigned    flavor         = k_flavor_lua;   /* D6：默认 Lua 5.5 */
    bool        flavor_forced  = false;
    unsigned    variant        = k_variant_modern;
    bool        variant_forced = false;
    bool        list_only      = false;
    bool        quiet          = false;
    bool        help_requested = false;
    bool        version_only   = false;
    const char *module_dir     = nullptr;
    const char *script         = nullptr;

    /* T1（D50）：`--json` / `--out=<path>`（机器可读结果信封）。`--out` 单独给也算 JSON 模式。
     * ❗与 `--server` 互斥（同时给 → 启动器报错、退出码 2）；JSON 模式下不弹文件对话框，
     *   且启动器自己的诊断走 stderr（stdout 必须被那一行 JSON 独占）。 */
    bool        json_mode      = false;
    static char g_json_out[260];

    /* T3（D42）：客户端模式（`--remote`）与它的控制命令。
     * ❗`--cancel` / `--kill` / `--detach` / `--health` / `--version` 只在**带** `--remote` 时
     *   才是「发帧给 server」；不带时 `--version` 仍是启动器自己的版本，其余报错。 */
    const char *remote_host  = nullptr;
    unsigned    remote_port  = 0;
    unsigned    remote_cmd   = 0;   /* 0 = 跑脚本；1..5 见 boot.remote_cmd */
    unsigned    client_pid   = 0;
    unsigned    read_timeout = 0;
    unsigned    max_frame    = 0;
    static char g_remote_arg[160];   /* 原字符串（host:port） */
    static char g_remote_host[128];  /* 拆出来的 host */

    /* S0（plan_server.md）：server / 自测模式的选项；字符串拷到静态缓冲，
     * 保证在 `autodbg_run` 调用期间有效。 */
    bool        server_mode    = false;
    unsigned    server_mode_v  = 1;   /* 1 = http（将被 D41 删除）、2 = raw tcp（D42） */
    bool        selftest_mode  = false;
    /* T7 切片 D1：`--repl`（不需要脚本；与 --json/--out、--server 互斥） */
    bool        repl_mode      = false;
    const char *server_addr    = nullptr;
    unsigned    server_port    = 0;
    unsigned    server_max_body = 0;
    unsigned    server_queue   = 0;
    unsigned    server_timeout = 0;
    unsigned    server_no_echo = 0;
    static char g_server_addr[128];
    static char g_server_token[128];
    static char g_server_log[260];
    static char g_server_eff[128];

    for (int i = 1; i < argc; i++)
    {
        const char *a = argv[i];

        if (starts_with(a, "--flavor="))
        {
            const char *v = a + 9;
            if (str_ieq(v, "lua") || str_ieq(v, "lua55") || str_ieq(v, "lua5.5"))
                flavor = k_flavor_lua;
            else if (str_ieq(v, "luajit") || str_ieq(v, "jit"))
                flavor = k_flavor_luajit;
            else
            {
                out_err("launcher: unknown --flavor value\n");
                return 1;
            }
            flavor_forced = true;
        }
        else if (starts_with(a, "--variant="))
        {
            const char *v = a + 10;
            if (str_ieq(v, "modern"))
                variant = k_variant_modern;
            else if (str_ieq(v, "legacy"))
                variant = k_variant_legacy;
            else
            {
                out_err("launcher: unknown --variant value\n");
                return 1;
            }
            variant_forced = true;
        }
        else if (starts_with(a, "--module-dir="))
        {
            module_dir = a + 13;
        }
        else if (str_eq(a, "--list"))
        {
            list_only = true;
        }
        else if (str_eq(a, "--quiet"))
        {
            quiet = true;
        }
        else if (str_eq(a, "--help") || str_eq(a, "-h") || str_eq(a, "-?"))
        {
            help_requested = true;
        }
        else if (str_eq(a, "--version"))
        {
            version_only = true;
        }
        else if (str_eq(a, "--selftest"))
        {
            selftest_mode = true;
        }
        else if (str_eq(a, "--repl"))
        {
            repl_mode = true;
        }
        else if (str_eq(a, "--json"))
        {
            json_mode = true;
        }
        else if (str_eq(a, "--remote") || starts_with(a, "--remote="))
        {
            /* `--remote=h:p` 或 `--remote h:p` */
            const char *v = (a[8] == '=') ? (a + 9) : nullptr;
            if (v == nullptr)
            {
                if (i + 1 >= argc)
                {
                    out_err("launcher: --remote needs <host:port>\n");
                    return 2;
                }
                v = argv[++i];
            }
            g_remote_arg[0] = '\0';
            append(g_remote_arg, sizeof g_remote_arg, v);
            if (!split_host_port(g_remote_arg, g_remote_host, sizeof g_remote_host, &remote_port))
            {
                out_err("launcher: --remote needs <host:port>\n");
                return 2;
            }
            remote_host = g_remote_host;
            if (remote_port == 0)
                remote_port = 8642;   /* 与 server 默认端口一致 */
        }
        else if (starts_with(a, "--pid="))
        {
            if (!parse_udec(a + 6, &client_pid) || client_pid == 0)
            {
                out_err("launcher: --pid=<n> expects a positive number\n");
                return 2;
            }
        }
        else if (starts_with(a, "--read-timeout="))
        {
            if (!parse_udec(a + 15, &read_timeout))
            {
                out_err("launcher: --read-timeout=<ms> expects a number\n");
                return 2;
            }
        }
        else if (starts_with(a, "--max-frame="))
        {
            if (!parse_udec(a + 12, &max_frame))
            {
                out_err("launcher: --max-frame=<bytes> expects a number\n");
                return 2;
            }
        }
        else if (str_eq(a, "--cancel") || str_eq(a, "--kill") || str_eq(a, "--detach") || str_eq(a, "--health"))
        {
            remote_cmd = str_eq(a, "--cancel")   ? 1u
                         : str_eq(a, "--kill")   ? 2u
                         : str_eq(a, "--detach") ? 3u
                                                  : 4u;
        }
        else if (starts_with(a, "--out="))
        {
            g_json_out[0] = '\0';
            append(g_json_out, sizeof g_json_out, a + 6);
            json_mode = true;   /* `--out` 单独给也生成信封（内容写文件、stdout 留空） */
        }
        else if (str_eq(a, "--server") || starts_with(a, "--server="))
        {
            const char *v = (a[8] == '=') ? (a + 9) : "";
            /* T5（D41/D42）：HTTP 传输已删除 —— `--server` / `--server=tcp` 都是 raw TCP 帧；
             * `--server=http` 明确报错（不静默降级，否则旧脚本会连上一个完全不同的协议）。 */
            if (str_ieq(v, "") || str_ieq(v, "tcp"))
            {
                server_mode   = true;
                server_mode_v = 2;
            }
            else if (str_ieq(v, "http"))
            {
                out_err("launcher: --server=http was removed (D41); use --server=tcp\n");
                return 2;
            }
            else
            {
                out_err("launcher: --server=<tcp> only\n");
                return 2;
            }
        }
        else if (starts_with(a, "--server-addr="))
        {
            g_server_addr[0] = '\0';
            append(g_server_addr, sizeof g_server_addr, a + 14);
            server_addr = g_server_addr;
        }
        else if (starts_with(a, "--server-port="))
        {
            if (!parse_u32(a + 14, &server_port, 1, 65535))
            {
                out_err("launcher: bad --server-port\n");
                return 1;
            }
        }
        else if (starts_with(a, "--server-token="))
        {
            g_server_token[0] = '\0';
            append(g_server_token, sizeof g_server_token, a + 15);
        }
        else if (starts_with(a, "--remote-token="))
        {
            /* T3：客户端握手用的 token；落到**同一个**字段（语义相同：握手帧里那几个 hex） */
            g_server_token[0] = '\0';
            append(g_server_token, sizeof g_server_token, a + 15);
        }
        else if (starts_with(a, "--server-max-body="))
        {
            if (!parse_u32(a + 18, &server_max_body, 1, 0x7FFFFFFFu))
            {
                out_err("launcher: bad --server-max-body\n");
                return 1;
            }
        }
        else if (starts_with(a, "--server-queue="))
        {
            if (!parse_u32(a + 15, &server_queue, 1, 256))
            {
                out_err("launcher: bad --server-queue\n");
                return 1;
            }
        }
        else if (starts_with(a, "--server-timeout="))
        {
            if (!parse_u32(a + 17, &server_timeout, 0, 86400))
            {
                out_err("launcher: bad --server-timeout\n");
                return 1;
            }
        }
        else if (starts_with(a, "--server-log="))
        {
            g_server_log[0] = '\0';
            append(g_server_log, sizeof g_server_log, a + 13);
        }
        else if (str_eq(a, "--server-no-echo"))
        {
            server_no_echo = 1;
        }
        else if (starts_with(a, "--"))
        {
            out_err("launcher: unknown option\n");
            return 1;
        }
        else if (script == nullptr)
        {
            script = a;
        }
        /* 其余位置参数属于脚本（模块侧按 boot.cmdline_a 重新拆一次，拿到全部） */
    }

    /* ---- T1（D50）：`--json` / `--out` 与 `--server` 互斥（同时给 → 报错、退出码 2）：
     *      server 的输出口径是帧（信封走 `result` 帧），与「stdout 只有一行 JSON」不能共存。 */
    if (json_mode && server_mode)
    {
        out_err("launcher: --json/--out cannot be combined with --server\n");
        return 2;
    }

    /* ---- T7 切片 D1：`--repl` 的两条互斥 ----
     * ① `--json` / `--out`：D50 要求 stdout **只有那一行**，REPL 的输出没法塞进信封；
     * ② `--server`：REPL 跑在 server 上（帧 + 命令归因）是切片 D2 的活，本切片只报错、不静默忽略。 */
    if (repl_mode)
    {
        if (json_mode)
        {
            out_err("launcher: --repl cannot be combined with --json/--out\n");
            return 2;
        }
        if (server_mode)
        {
            out_err("launcher: --repl with --server lands in D2\n");
            return 2;
        }
    }

    /* ---- T3（D42）：客户端模式的校验 ----
     * `--version` 在**不带** `--remote` 时保持老语义（打启动器版本、退出 0）；带了就变成
     * 「向 server 查版本」的控制命令。控制命令离开 `--remote` 就是无意义的 → 报错。 */
    if (remote_host == nullptr)
    {
        if (remote_cmd != 0)
        {
            out_err("launcher: --cancel/--kill/--detach/--health need --remote\n");
            return 2;
        }
    }
    else if (version_only)
    {
        version_only = false;   /* 改成查 server 版本 */
        remote_cmd   = 5u;
    }

    /* ---- `--help` / `--version`：不碰载荷、不弹对话框，直接打印退出 ---- */
    if (help_requested)
    {
        print_usage();
        return 0;
    }
    if (version_only)
    {
        print_version();
        return 0;
    }

    /* ---- 定脚本（D7：无参时弹文件对话框） ---- */
    if (script != nullptr)
    {
        set_script_ansi(script);
    }
    else if (!list_only && !server_mode && !selftest_mode && !repl_mode && !json_mode && !(remote_host != nullptr && remote_cmd != 0))
    {
        /* JSON 模式不弹窗：它是机器可读接口，拿不到交互桌面时应该直接报错（退出码 -1）；
         * 客户端的控制命令（cancel/kill/detach/health/version）也不需要脚本；
         * `--repl` 同理 —— 它的输入是 stdin，没有脚本。 */
        if (!ask_script())
            return (GetLastError() == 0) ? 0 : -1;   /* 取消 = 正常退出 */
        script = g_script_a;
    }

    /* ---- 定 flavor：CLI > 脚本标记 > 默认（D10） ---- */
    if (!flavor_forced && script != nullptr)
    {
        char     bad[16];
        unsigned r = flavor_from_script(script, bad, sizeof bad);

        if (r == 3)
        {
            out_err("launcher: unknown script marker --!");
            out_err(bad);
            out_err("\n");
            return 1;
        }
        if (r <= 1)
            flavor = r;
    }

    /* ---- 定 variant（§2.6） ---- */
    if (!variant_forced)
        variant = detect_variant();

    /* ---- 载荷 ---- */
    payload    pack;
    load_error err;

    err.clear();
    if (!pack.open(g_packdata, g_packdata_size, &err))
    {
        report("launcher: payload open failed", err);
        return 2;
    }

    unsigned char *raw = (unsigned char *)autodbg::common::alloc(pack.total_raw_size());
    if (!pack.extract_all(raw, pack.total_raw_size(), &err))
    {
        report("launcher: payload extract failed", err);
        return 2;
    }

    if (list_only)
    {
        out("launcher: entries=");
        out_u32(pack.count());
        out(" raw_total=");
        out_u32(pack.total_raw_size());
        out(" payload=");
        out_u32(pack.payload_size());
        out("\n");
        for (unsigned i = 0; i < pack.count(); i++)
        {
            char     name[64];
            lower_copy(name, sizeof name, pack.name(i), pack.name_len(i));
            out("launcher:   ");
            out(name);
            out(" size=");
            out_u32(pack.raw_size(i));
            out("\n");
        }
        out("launcher: flavor=");
        out(flavor == k_flavor_lua ? "lua" : "luajit");
        out(flavor_forced ? " (forced)" : "");
        out(" variant=");
        out(variant == k_variant_modern ? "modern" : "legacy");
        out(variant_forced ? " (forced)" : " (auto)");
        out("\n");
        return 0;
    }

    /* ---- 取模块字节：优先 --module-dir，否则从载荷切片 ---- */
    unsigned char *disk_buf  = nullptr;
    unsigned       disk_size = 0;

    if (module_dir != nullptr)
    {
        char module_name[64];
        build_module_name(module_name, sizeof module_name, flavor, variant);

        char path[260];
        path[0] = '\0';
        append(path, sizeof path, module_dir);
        append(path, sizeof path, "\\");
        append(path, sizeof path, module_name);

        disk_buf = read_file(path, &disk_size);
        if (disk_buf == nullptr)
        {
            out_err("launcher: cannot read module from --module-dir\n");
            return 2;
        }
        /* JSON 模式：启动器自己的诊断不能进 stdout（那一行 JSON 必须独占）→ 走 stderr */
        if (json_mode)
            out_err("launcher: module from disk (--module-dir)\n");
        else
            out("launcher: module from disk (--module-dir)\n");
    }

    /* ---- 映射 + 启动：modern 失败则回退 legacy（F12；`--variant` 强制时不回退） ---- */
    mapped_module mod;
    void *        run_ptr      = nullptr;
    unsigned      used_variant = variant;

    for (unsigned attempt = 0; attempt < 2; attempt++)
    {
        unsigned v         = (attempt == 0) ? variant : (1u - variant);
        bool     can_retry = (!variant_forced && attempt == 0);
        char     module_name[64];

        build_module_name(module_name, sizeof module_name, flavor, v);

        const void *module_data = nullptr;
        unsigned    module_size = 0;

        if (disk_buf != nullptr)
        {
            /* --module-dir 只准备了一个变体（按用户选定/探测的那个） */
            if (v != variant)
                break;
            module_data = disk_buf;
            module_size = disk_size;
        }
        else
        {
            int idx = pack.find(module_name);
            if (idx < 0)
            {
                if (v == variant)
                {
                    out_err("launcher: module not in payload: ");
                    out_err(module_name);
                    out_err("\n");
                    if (!quiet)
                        MessageBoxA(NULL, "载荷里没有所需的模块。", "AutoDbg", MB_OK | MB_ICONERROR);
                }
                if (!can_retry)
                    return 2;
                continue;
            }
            module_data = raw + pack.raw_offset((unsigned)idx);
            module_size = pack.raw_size((unsigned)idx);
        }

        err.clear();
        if (!mod.map(module_data, module_size, nullptr, &err))
        {
            report("launcher: map failed", err);
            if (!can_retry)
            {
                if (!quiet)
                    MessageBoxA(NULL, "模块映射失败，详见控制台输出。", "AutoDbg", MB_OK | MB_ICONERROR);
                return 2;
            }
            out_err("launcher: falling back to the other variant\n");
            continue;
        }

        run_ptr = mod.resolve("autodbg_run");
        if (run_ptr == nullptr)
        {
            out_err("launcher: module does not export autodbg_run\n");
            if (!can_retry)
            {
                if (!quiet)
                    MessageBoxA(NULL, "模块未导出 autodbg_run。", "AutoDbg", MB_OK | MB_ICONERROR);
                return 2;
            }
            mod.unmap();
            continue;
        }

        used_variant = v;
        break;
    }

    if (run_ptr == nullptr)
    {
        out_err("launcher: no usable module\n");
        return 2;
    }

    autodbg_run_fn run = (autodbg_run_fn)run_ptr;

    /* `boot`：exe ↔ DLL 唯一契约（§2.3） */
    autodbg_boot boot;
    memset(&boot, 0, sizeof boot);
    boot.abi        = AUTODBG_ABI_VERSION;
    boot.size       = sizeof boot;
    boot.host_image = (void *)GetModuleHandleA(NULL);
    boot.cmdline_a  = GetCommandLineA();
    boot.cmdline_w  = query_cmdline_w();
    boot.script_a   = (g_script_a[0] != '\0') ? g_script_a : nullptr;
    boot.script_w   = (g_script_w[0] != 0) ? g_script_w : nullptr;
    boot.flavor     = flavor;
    boot.variant    = used_variant;

    /* ---- S0（D27）：server / 自测模式 ---- */
    boot.server_mode = server_mode ? server_mode_v : 0u;
    boot.selftest    = selftest_mode ? 1u : 0u;
    boot.server_addr = nullptr;
    if (server_port != 0)
    {
        /* `--server-port` 覆盖 addr 里的端口（与参数顺序无关） */
        build_addr(g_server_eff, sizeof g_server_eff, server_addr, server_port);
        boot.server_addr = g_server_eff;
    }
    else if (server_addr != nullptr)
    {
        boot.server_addr = server_addr;
    }
    boot.server_token       = (g_server_token[0] != '\0') ? g_server_token : nullptr;
    boot.server_max_body    = server_max_body;
    boot.server_queue       = server_queue;
    boot.server_timeout_sec = server_timeout;
    boot.server_log         = (g_server_log[0] != '\0') ? g_server_log : nullptr;
    boot.server_no_echo     = server_no_echo;

    /* ---- T1（D50）：JSON 信封（ABI 3）---- */
    boot.json_mode = json_mode ? 1u : 0u;
    boot.json_out  = (g_json_out[0] != '\0') ? g_json_out : nullptr;

    /* ---- T7 切片 D1：`--repl`（ABI 5）---- */
    boot.repl_mode = repl_mode ? 1 : 0;

    /* ---- T3（D42）：客户端（ABI 4）---- */
    boot.remote_host     = remote_host;
    boot.remote_port     = remote_port;
    boot.remote_cmd      = remote_cmd;
    boot.pid             = client_pid;
    boot.read_timeout_ms = read_timeout;
    boot.max_frame       = max_frame;
    if (remote_host != nullptr)
    {
        /* 客户端模式的 token 复用 server_token 字段（同一份语义）；未给 → D21 缺省
         * （loopback 不校验，非 loopback 由 server 拒） */
        boot.server_token = (g_server_token[0] != '\0') ? g_server_token : nullptr;
    }

    int rc = run(&boot);

    if (disk_buf != nullptr)
        autodbg::common::release(disk_buf);
    mod.unmap();
    return rc;
}
