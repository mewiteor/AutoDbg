#ifndef AUTODBG_CORE_UTIL_HPP
#define AUTODBG_CORE_UTIL_HPP
/* =============================================================================
 * src/core/util.hpp —— 核心通用工具（Plan P3.A.3 / 5.7）
 *
 * 来源：`src/debugger/lib.c` 的**非 Lua 部分**（打印/错误、UTF 转换、CRC32、trim、ANSI shim）。
 * 拆分理由：`lib.h` 同时 include `<lua.h>`，使调试器核心被**传递性**绑定到 Lua（TODO 4.6）。
 *
 * 与旧实现的差异（仅此两类，其余 1:1 逐字保留）：
 *   ① `_Generic` 宏 → **同名重载函数**（C++ 没有 `_Generic`；Plan P6.3 的提前落地）。
 *      这样所有调用点（`Print`/`ErrorS`/`ErrorF`/…）**一字不改**，`TCHAR` 的
 *      ANSI/Unicode 二态由重载解析自动选择（不再需要 `_Generic`）。
 *   ② `alloc_`/`free_` 宏 → `autodbg::common::alloc`/`release`（Plan P2.1）。
 *
 * ❗输出分流（P7，见 `plan_p7_win9x.md` §2.4）：控制台走 `WriteConsole`（不受代码页
 *   影响，保证不乱码），非控制台（文件/管道）走 `WriteFile` —— 原来重定向时
 *   `WriteConsole` 直接失败，输出静默丢失。ANSI（legacy）变体一律 `WriteFile`：Win9x
 *   的控制台是 ANSI-only，没有 Unicode 支持，按执行字符集（CP936）写字节。
 *   这是唯一一处偏离 1:1；旧 `lib.c` 与新 `util.cpp` 的逐字节比对测试需要为此加例外。
 *
 * ❗再一处（S0，server 模式）：`PrintExN` 顶部多一个 **输出 sink**（`set_output_sink`）；
 *   装了 sink 且它返回「已消费」时直接返回、不写句柄（`plan_server.md` §5.3）。
 *   未装 sink 时行为与 P7 完全一致。
 *
 * ❗窄串的控制台分支**不能**用 `WriteConsoleA`：UNICODE 构建的窄串是 **UTF-8**
 *   （`/utf-8` 同时改了执行字符集），`WriteConsoleA` 会按控制台代码页（中文 = CP936）
 *   解释这些字节 → 中文全乱（`server: 配置错误 …` 显示成 `閰嶇疆閿欒 …`）。
 *   现在先 `utf8_to_utf16_n()` 再 `WriteConsoleW`（见 `PITFALLS.md` O1b）。
 *
 * 注意：`get_lua_str` / `lua_type_error` / `str2integer` 属 Lua 绑定层，见
 *   `src/scripting/lua/helpers.hpp`。
 * ========================================================================== */
#include <windows.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>

/* 分配入口（P2.1）：统一走 common 的分配器 */
#include "../common/allocator.hpp"

namespace autodbg::core
{
/* ------------------------------ 输出 ------------------------------ */
/* 低层：直接写控制台句柄（替代旧的 PrintExN 宏） */
void PrintExN(DWORD handle, const char *s, size_t n);
void PrintExN(DWORD handle, const wchar_t *s, size_t n);

/* ------------------- 输出 sink（server 模式，plan_server.md D22 / §5.3） -------------------
 * `PrintExN` 是本层**所有**输出（`Print*` / `Error*` / `log_msg` / `ErrorMessage` / `ERR`）
 * 的唯一出口；server 模式在这里把输出收进「当前请求」的缓冲。约定：
 *   · `stream`：1 = stdout（STD_OUTPUT_HANDLE）、2 = stderr（STD_ERROR_HANDLE）；
 *   · `utf8` / `len`：**UTF-8 无 BOM**、不含结尾 NUL（与 plan_p7_win9x.md §2.4 的重定向口径一致）；
 *   · 返回 true = 已消费（不再写句柄），false = 继续走默认输出（控制台/文件分流）。
 * sink 只应由宿主层在**单线程**上下文中安装/卸载（引擎本身不设 sink）。
 * ❗ sink 在「没有合法 std 句柄」时**仍然会被调用**（server 里通常没有控制台），
 *   所以它必须排在 `GetStdHandle` 检查之前。 */
typedef bool (*output_sink_fn)(void *ctx, int stream, const char *utf8, size_t len);

void set_output_sink(output_sink_fn fn, void *ctx);

/* 当前是否已装 sink（宿主层自测/断言用） */
bool output_sink_active();

/* ------------------- 子进程 std 句柄策略（T1 / D50） -------------------
 * 默认（false）= 旧行为：被调试进程**继承本进程的 std 句柄**，它的 console 输出与控制台/重定向同流。
 * 开启（true）= `run()` 里 `CreateProcess` 时用 `STARTF_USESTDHANDLES` 把目标的
 *   stdout/stderr 指到**本进程 stderr 的可继承副本**（stdin 指 NUL）：
 *   · stdout 因此干净 —— `--json` 的那一行 JSON 不会被目标输出顶掉（D50）；
 *   · 不丢目标输出（仍落在 stderr 流里），也不需要有人排空；
 *   · ❗不用匿名管道：管道必须有人持续读，而 `--detach` 之后没人能读 —— 关读端会让目标的
 *     下一次 write 拿到 `ERROR_BROKEN_PIPE`，留着读端不读又会让它填满 4 KB 后阻塞，两者都
 *     违背 D47「detach 后目标继续活着」；副本句柄则随目标自己活，本进程关掉自己那份即可。
 * ❗代价：`bInheritHandles` 必须为 TRUE（Win9x 没有 `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`），
 *   所以本进程的其它**可继承**句柄也会被交给目标。只在 `--json` 下开启。 */
void set_child_stdio_capture(bool enable);
bool child_stdio_capture();

/* 变参底层（替代旧的 PrintExA/W/VA/VW 宏 + 函数） */
void PrintExA(DWORD o, LPCSTR fmt, ...);
void PrintExVA(DWORD o, LPCSTR fmt, va_list ap);
#ifdef _UNICODE
void PrintExW(DWORD o, LPCWSTR fmt, ...);
void PrintExVW(DWORD o, LPCWSTR fmt, va_list ap);
#endif

/* 中间层（替代旧的 PrintExS / PrintEx 宏；一律按字符串长度输出） */
inline void PrintExS(DWORD handle, const char *s) { PrintExN(handle, s, strlen(s)); }
inline void PrintExS(DWORD handle, const wchar_t *s) { PrintExN(handle, s, wcslen(s)); }
inline void PrintEx(DWORD handle, const char *s) { PrintExN(handle, s, strlen(s)); }
inline void PrintEx(DWORD handle, const wchar_t *s) { PrintExN(handle, s, wcslen(s)); }

/* 流式打印（替代旧的 PrintN/PrintS/Print 系列宏） */
inline void PrintN(const char *s, size_t n) { PrintExN(STD_OUTPUT_HANDLE, s, n); }
inline void PrintN(const wchar_t *s, size_t n) { PrintExN(STD_OUTPUT_HANDLE, s, n); }
inline void PrintS(const char *s) { PrintExS(STD_OUTPUT_HANDLE, s); }
inline void PrintS(const wchar_t *s) { PrintExS(STD_OUTPUT_HANDLE, s); }
inline void Print(const char *s) { PrintEx(STD_OUTPUT_HANDLE, s); }
inline void Print(const wchar_t *s) { PrintEx(STD_OUTPUT_HANDLE, s); }

inline void ErrorN(const char *s, size_t n) { PrintExN(STD_ERROR_HANDLE, s, n); }
inline void ErrorN(const wchar_t *s, size_t n) { PrintExN(STD_ERROR_HANDLE, s, n); }
inline void ErrorS(const char *s) { PrintExS(STD_ERROR_HANDLE, s); }
inline void ErrorS(const wchar_t *s) { PrintExS(STD_ERROR_HANDLE, s); }
inline void Error(const char *s) { PrintEx(STD_ERROR_HANDLE, s); }
inline void Error(const wchar_t *s) { PrintEx(STD_ERROR_HANDLE, s); }

/* 格式化打印（替代旧的 PrintF/ErrorF/ErrorFV 宏；变参无法用内联转发 → 真实函数）
 * 注：**宽字符格式化路径仅 _UNICODE 构建存在**（1:1 对应旧 lib.c 的 `#ifdef _UNICODE`
 *   PrintExW/PrintExVW）；在 ANSI(98) 构建下用宽串调它会得到**编译期错误**，
 *   而不是静默无输出（旧实现是链接期错误）。窄字符路径两边都有。 */
void PrintF(const char *fmt, ...);
void ErrorF(const char *fmt, ...);
void ErrorFV(const char *fmt, va_list ap);
#ifdef _UNICODE
void PrintF(const wchar_t *fmt, ...);
void ErrorF(const wchar_t *fmt, ...);
void ErrorFV(const wchar_t *fmt, va_list ap);
#endif

/* 日志：`[文件][时间][行号] 格式化文本\n`（替代旧的 log_msg + LOG 宏） */
void log_msg(const char *file, DWORD line, const char *fmt, ...);
void log_msg(const wchar_t *file, DWORD line, const wchar_t *fmt, ...);

/* 错误报告（替代旧的 ErrorMessage + ERR/E 宏）：打印文件/行号/函数/错误码/系统错误文本 */
void ErrorMessage(const char *file_name, DWORD line, const char *parent_function, const char *function, DWORD code, ...);
#ifdef _UNICODE
void ErrorMessage(const wchar_t *file_name, DWORD line, const wchar_t *parent_function, const wchar_t *function, DWORD code, ...);
#endif

/* ------------------------------ CRC32 ------------------------------ */
void init_tab();
bool calc_crc32(LPCTSTR path, DWORD *crc32);

/* ------------------------- UTF / ANSI 转换 ------------------------- */
/* 语义（**必须 1:1 保留**，见 tests/unit/test_utf.c 与差分测试）：
 *   *_n 变体的 `*n` 是**输入长度**，返回新分配缓冲（需 common::release），NUL 终止，
 *   且 `*n` 被改写为**输出长度**（utf16 为 WCHAR 数、utf8 为字节数）。
 *   例外：`ansi_to_utf8_n` 沿用旧实现 —— **不更新 `*n`、也不写 NUL**（缺陷 D12，本轮只记录）。 */
LPWSTR utf8_to_utf16(LPCSTR s, int *n);
LPSTR  utf16_to_utf8(LPCWSTR s, int *n);
LPWSTR utf8_to_utf16_n(LPCSTR s, int *n);
LPSTR  utf16_to_utf8_n(LPCWSTR s, int *n);
LPSTR  ansi_to_utf8_n(LPCSTR s, int *n);

/* 按构建配置的 TCHAR 包装（替代 lib.h 的同名宏） */
#ifdef _UNICODE
#   define tchar_to_utf8(s)   utf16_to_utf8(s, NULL)
#   define utf8_to_tchar(s)   utf8_to_utf16(s, NULL)
#   define free_from_tchar(p) autodbg::common::release(p)
#else
#   define tchar_to_utf8(s)   (s)
#   define utf8_to_tchar(s)   (s)
#   define free_from_tchar(p) ((void)0)
#endif

/* ------------------------------ 字符串 ------------------------------ */
/* 去掉首尾的空白/控制字符（原地修改，返回新的起始指针） */
char *trim(char *s);

/* ------------------------ 非 Unicode 兼容 shim ------------------------ */
/* 旧 lib.h 在 Unicode 构建下把它们 #define 到真 API；这里换成等价的内联包装，
 * 保证调用点（如 run 里的 LjDebugSetProcessKillOnExit(FALSE)）不变。 */
#ifdef _UNICODE
inline BOOL LjDebugSetProcessKillOnExit(BOOL KillOnExit) { return DebugSetProcessKillOnExit(KillOnExit); }
inline HWND LjGetConsoleWindow() { return GetConsoleWindow(); }
#else
BOOL  LjDebugSetProcessKillOnExit(BOOL KillOnExit);
HWND  LjGetConsoleWindow();
DWORD LjGetMappedFileNameA(DWORD dwPid, LPVOID lpv, LPSTR lpFilename, DWORD nSize);
#endif

/* ---------------------- 老系统（Win95）API 等价包装 ---------------------- */
/* 这几个 user32 增强 API **不在 Win95 的 user32.dll 里**（Win98/NT4 才有）。静态导入会让
 * legacy 变体在 Win95 上映射就失败（手工映射器解析导入是硬失败，见 module_loader）→
 * 一律「动态解析 + 等价回落」，调用点不变。实现见 util.cpp。 */
HWND LjGetRootWindow(HWND hwnd); /* = GetAncestor(hwnd, GA_ROOT) */
BOOL LjFlashWindow(HWND hwnd);   /* = FlashWindowEx(FLASHW_ALL|FLASHW_TIMERNOFG, uCount 3) */

/* ------------------------------ 便捷宏 ------------------------------ */
/* 保留旧名字（迁移期对照用；宏只做转发，语义与旧 lib.h 一致） */
#define ZeroMemN(s, n)    __stosb((PBYTE)(s), 0, (n) * sizeof(*(s)))
#define ZeroMem(s)        __stosb((PBYTE) & (s), 0, sizeof(s))
#define CopyMem(d, s, n)  __movsb((PBYTE)(d), (const BYTE *)(s), (n))
#define CopyMem2(d, s, n) __movsw((PWORD)(d), (const WORD *)(s), (n))
#define CopyMem4(d, s, n) __movsd((PDWORD)(d), (const DWORD *)(s), (n))
#define CopyMem8(d, s, n) __movsq((PQWORD)(d), (const QWORD *)(s), (n))
#define AllocX(T, count)  autodbg::common::alloc_array<T>(count)
#define DV(v)             (v) && (v)

/* ------------------- 过渡期桥接：旧分配宏 → 新分配器 -------------------
 * `alloc_`/`free_` 是旧 lib.h 里直接映射 malloc/free 的宏，在迁移中的
 * 文件里出现数百次。P3.A 阶段先用同名桥接避免巨量改名（P3.B/P6 再逐处清理）。
 * 语义差异（已知、可接受）：`common::alloc` 在 OOM 时 fail-fast（ExitProcess(3)），
 * 而旧 `malloc` 返回 NULL —— 旧代码绝大多数调用点本就不检查 NULL。 */
#define alloc_(size)       autodbg::common::alloc(size)
#define free_(p)           autodbg::common::release(p)
#ifdef UNICODE
#   define CopyMemT CopyMem2
#else
#   define CopyMemT CopyMem
#endif
#define CopyTStr(d, s) CopyMemT(d, s, _countof(s))

#define LOG(...)          autodbg::core::log_msg(TEXT(__FILE__), __LINE__, __VA_ARGS__)
#define ERR(fn, code, ...) \
    autodbg::core::ErrorMessage(TEXT(__FILE__), __LINE__, TEXT(__FUNCTION__), TEXT(fn), code __VA_OPT__(, ) __VA_ARGS__)
#define E(fn, ...)        ERR(fn, GetLastError() __VA_OPT__(, ) __VA_ARGS__)
}

#endif /* AUTODBG_CORE_UTIL_HPP */
