/* =============================================================================
 * src/sys/pack/handoff.cpp —— handoff 的执行面（口径见 `handoff.hpp` 的头注释）
 *
 * 职责：落盘（逐字节校验）→ 设 env `AUTODBG_BASE_DIR` → 拼 child 的 command line（引号口径）→
 *       CreateProcess + job → wait → 把 child 的退出码原样交回去。`launcher.cpp` 解析完选项后
 *       调 `run_handoff()`，其它事情都不在这里发生。
 * 来源：新写（plan_bootloader.md §2 / §5.5 的 (a)）。
 * 与旧实现的差异：无（新文件）。与 launcher 的其余部分不同：这里是**进程边界**，不组装
 *   `autodbg_boot`（ABI 保持 5）——child 是独立跑的产物，它自己重新拆自己的命令行。
 *
 * 零 CRT：分配走 `autodbg::common::alloc`；不用 `std::`；打印走本文件的 `write_err`（§3：
 *   启动器输出一律 ASCII，且同一条输出必须走同一个流，PITFALLS O2）。
 * 符号口径（plan_bootloader.md §3 的实测表）：w95 就有的直接链（WriteFile / ReadFile / CreateFileW /
 *   SetEndOfFile / GetTempPathA / WaitForSingleObject / GetExitCodeProcess / CloseHandle /
 *   GetModuleFileNameA / SetEnvironmentVariableA / MultiByteToWideChar / WideCharToMultiByte /
 *   GetStdHandle / GetLastError —— 后四个已由 launcher.cpp 在同一份产物里引入）；
 *   `IsWow64Process` 与 job 三条（只有 wxp）一律 dynapi；W 版 API 照 launcher.cpp 的现有写法
 *   走 `GetProcAddress` + A 版降级（`GetTempPathW` / `CreateProcessW` → `CreateProcessA`）；
 *   `CreateProcessA` 也走 dynapi，以免给启动器新增导入。
 *   `PathFindFileNameA` 虽然也在 w95 集合里但**不用**：它在 shlwapi，会新增一个导入库（分目录自己切）。
 * 弹窗口径（与 launcher 一致，已定）：`--quiet` 只写 stderr，否则同一个文本也弹窗 —— 走
 *   `report_err` 这一个包装（PITFALLS O9），`MessageBoxA` 已由 launcher.cpp 在同一份产物里引入。
 * ========================================================================== */
#include <windows.h>
#include <string.h>   /* memset（影子头） */

#include "../../common/allocator.hpp"
#include "../../common/dynapi.hpp"
#include "handoff.hpp"

using autodbg::common::get_proc_as;

namespace autodbg::sys::pack
{
namespace
{

/* CreateProcess 的命令行上限：32767 字符 + NUL（与 pack_entry.c 的 s_cmdline 同口径） */
constexpr unsigned k_cmdline_cap = 32768;

/* ❗函数指针 typedef 必须与真实 API 逐参数一致、且带 WINAPI（PITFALLS K11：参数个数或调用约定
 *   不对，x86 上一调就 0xC0000005） */
typedef BOOL(WINAPI *get_temp_path_w_fn)(DWORD n, wchar_t *dst);
typedef BOOL(WINAPI *create_process_w_fn)(const wchar_t *app, wchar_t *cmdline,
                                          SECURITY_ATTRIBUTES *sa, SECURITY_ATTRIBUTES *sa2,
                                          BOOL inherit, DWORD flags, void *env, const wchar_t *cur,
                                          STARTUPINFOW *si, PROCESS_INFORMATION *pi);
typedef BOOL(WINAPI *create_process_a_fn)(const char *app, char *cmdline,
                                          SECURITY_ATTRIBUTES *sa, SECURITY_ATTRIBUTES *sa2,
                                          BOOL inherit, DWORD flags, void *env, const char *cur,
                                          STARTUPINFOA *si, PROCESS_INFORMATION *pi);
typedef BOOL(WINAPI *is_wow64_process_fn)(HANDLE process, PBOOL wow64);
typedef HANDLE(WINAPI *create_job_object_w_fn)(SECURITY_ATTRIBUTES *sa, const wchar_t *name);
typedef BOOL(WINAPI *set_information_job_object_fn)(HANDLE job, int info_class, const void *info, DWORD size);
typedef BOOL(WINAPI *assign_process_to_job_object_fn)(HANDLE job, HANDLE process);

/* ---- 输出：诊断一律走 stderr（stdout 必须被 child 的 `--json` 那一行独占，D50）----
 * launcher.cpp 的 out / out_err 在匿名命名空间里（跨 TU 不可见），这里只重复 stderr 那一半。
 * ❗同一条输出必须走同一个流（PITFALLS O2），所以字符串与数字都从同一份缓冲里出。 */
void write_err(const char *s)
{
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h == NULL || h == INVALID_HANDLE_VALUE)
        return;

    unsigned len = 0;
    while (s[len] != '\0')
        len++;

    DWORD written = 0;
    WriteFile(h, s, len, &written, NULL);
}

/* 往行缓冲追加（限 cap，越界截断而不越界） */
void append_ascii(char *dst, unsigned cap, unsigned *pos, const char *s)
{
    while (*s != '\0' && *pos + 1 < cap)
        dst[(*pos)++] = *s++;
    dst[*pos] = '\0';
}

void append_u32(char *dst, unsigned cap, unsigned *pos, unsigned v)
{
    char tmp[16];
    int  i = 0;

    do
    {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v != 0);
    while (i > 0)
    {
        if (*pos + 1 >= cap)
            break;
        dst[(*pos)++] = tmp[--i];
    }
    dst[*pos] = '\0';
}

/* ❗唯一的错误出口（PITFALLS O9：弹窗必须走同一个包装函数）：stderr 一行 +（非 `--quiet` 时）弹窗。
 *   文本是同一份 ASCII（§3：启动器输出一律 ASCII）——不另写一套「弹窗专用文案」，否则两条路径会分叉。
 *   `s_quiet` 在 `run_handoff` 入口置位（handoff 是进程边界，拿不到 launcher 的局部变量，所以用文件作用域
 *   的 bool；这是 POD 静态，不是 §11.5 第 1 条禁的东西）。弹窗路径在无头门禁里不可断言（模态框），
 *   所以 `e2e_boot.ps1` 全程带 `--quiet`，断的是 stderr 那一半。 */
static bool s_quiet = false;

void report_err(const char *what, const wchar_t *path, unsigned code)
{
    char line[256];
    unsigned pos = 0;
    line[0] = '\0';

    append_ascii(line, (unsigned)sizeof line, &pos, "handoff: ");
    append_ascii(line, (unsigned)sizeof line, &pos, what);

    if (path != nullptr)
    {
        /* §3：启动器输出一律 ASCII（中文在 CP936 控制台上会乱码）→ 路径只在纯 ASCII 时才打，
         * 否则打固定占位；诊断本身（Win32 code）仍然可读。 */
        append_ascii(line, (unsigned)sizeof line, &pos, " path=");
        bool non_ascii = false;
        for (unsigned i = 0; path[i] != 0; i++)
        {
            if (path[i] > 0x7F)
            {
                non_ascii = true;
                break;
            }
            if (pos + 1 >= (unsigned)sizeof line)
                break;
            line[pos++] = (char)path[i];
            line[pos]   = '\0';
        }
        if (non_ascii)
            append_ascii(line, (unsigned)sizeof line, &pos, "(non-ascii path)");
    }

    if (code != 0)
    {
        append_ascii(line, (unsigned)sizeof line, &pos, " code=");
        append_u32(line, (unsigned)sizeof line, &pos, code);
    }

    write_err(line);
    write_err("\n");

    if (!s_quiet)
        MessageBoxA(NULL, line, "AutoDbg", MB_OK | MB_ICONERROR);
}

/* ---- 零 CRT 的字符串小工具（不用 strcpy / strcat / std::）---- */
bool append_w(wchar_t *dst, unsigned cap, unsigned *pos, const wchar_t *s)
{
    while (*s != 0)
    {
        if (*pos + 1 >= cap)
            return false;
        dst[(*pos)++] = *s++;
    }
    dst[*pos] = 0;
    return true;
}

bool append_w1(wchar_t *dst, unsigned cap, unsigned *pos, wchar_t c)
{
    if (*pos + 1 >= cap)
        return false;
    dst[(*pos)++] = c;
    dst[*pos] = 0;
    return true;
}

/* manifest 里的名字是 ASCII（小写、无路径、不带 NUL，由调用方拷成 C 串）⇒ 逐字符转宽。
 * 不能直接拿 append_w 用：类型不同（char* 与 wchar_t*），编译阶段就拒。 */
bool append_cstr_w(wchar_t *dst, unsigned cap, unsigned *pos, const char *s)
{
    while (*s != '\0')
    {
        if (*pos + 1 >= cap)
            return false;
        dst[(*pos)++] = (wchar_t)*s++;
    }
    dst[*pos] = 0;
    return true;
}

/* `autodbg_split_cmdline_w` 的**逆过程**（逐条对应，child 用同一个 splitter 拆，所以逐字节还原）：
 *   ① 反斜杠 run 后面是「要当字面量保留的引号」→ 写 2k+1 个（splitter 对 n 个反斜杠 + 引号：发 n/2 个反斜杠，
 *      n 为奇时把引号当字面量、n 为偶时它只是「开关引号」）——写 2k 会把引号当引号边界用掉，所以差一个；
 *   ② 串内的引号 → 写成 `\"`（奇数 run ⇒ 字面量）；
 *   ③ token 里有空白或引号 → 整体用引号包住；
 *   ④ token 末尾的反斜杠 run（闭合引号紧跟其后）→ 写 2k（偶数 run ⇒ 发 k 个反斜杠后正常闭合）。
 * 不写第二套拼接口径：argv[0] 之外的部分一律**逐字节原样透传**（见 after_first_token）。
 * ❗Windows 文件名里不能出现 `"` ⇒ ①② 对「路径类 token」实际不可达，这里是防御性实现（门禁测不到，
 *   门禁直接判的是空格与反斜杠：见 `handoff.cmdline-passthrough`）。 */
bool append_quoted_w(wchar_t *dst, unsigned cap, unsigned *pos, const wchar_t *token)
{
    bool need_quote = false;
    for (unsigned i = 0; token[i] != 0; i++)
    {
        if (token[i] == L' ' || token[i] == L'\t' || token[i] == L'"')
        {
            need_quote = true;
            break;
        }
    }
    if (!need_quote)
        return append_w(dst, cap, pos, token);

    unsigned total = 0;
    while (token[total] != 0)
        total++;
    unsigned trail = 0;
    while (trail < total && token[total - 1 - trail] == L'\\')
        trail++;

    if (*pos + 1 >= cap)
        return false;
    dst[(*pos)++] = L'"';

    unsigned i = 0;
    while (token[i] != 0)
    {
        if (token[i] == L'\\')
        {
            unsigned run = 0;
            while (token[i + run] == L'\\')
                run++;

            /* 后面是字面引号 → 2k+1（奇数 run 才让引号成为字面量）；这是末尾 run（闭合引号紧跟其后）
             * → 2k（偶数 run，发 k 个反斜杠后正常闭合） */
            unsigned emit = (token[i + run] == L'"') ? (run * 2 + 1) : ((run == trail) ? (run * 2) : run);
            for (unsigned k = 0; k < emit; k++)
            {
                if (*pos + 1 >= cap)
                    return false;
                dst[(*pos)++] = L'\\';
            }
            i += run;
            continue;
        }
        if (token[i] == L'"')
        {
            if (*pos + 2 >= cap)
                return false;
            dst[(*pos)++] = L'\\';
            dst[(*pos)++] = L'"';
            i++;
            continue;
        }
        if (*pos + 1 >= cap)
            return false;
        dst[(*pos)++] = token[i++];
    }

    if (*pos + 1 >= cap)
        return false;
    dst[(*pos)++] = L'"';
    dst[*pos] = 0;
    return true;
}

/* 第一个 token（argv[0]）之后的部分——按 splitter 的 argv[0] 规则走（引号内**不**处理反斜杠转义）。
 * 这部分原样透传：child 拆出来的结果与主 exe 拆出来的逐字节相同。 */
const wchar_t *after_first_token(const wchar_t *cmdline)
{
    const wchar_t *p = cmdline;

    while (*p == L' ' || *p == L'\t')
        p++;
    if (*p == L'"')
    {
        p++;
        while (*p != 0 && *p != L'"')
            p++;
        if (*p == L'"')
            p++;
    }
    else
    {
        while (*p != 0 && *p != L' ' && *p != L'\t')
            p++;
    }
    return p;
}

/* tmpdir（宽）：`GetTempPathW` 走 GetProcAddress（照 launcher.cpp 的写法），取不到降级
 * `GetTempPathA`（全 w95）。handoff 只在 64 位 host 上发生，所以实际路径恒是 W；A 分支是保险。 */
bool get_temp_path_w(wchar_t *dst, unsigned cap)
{
    get_temp_path_w_fn fn = get_proc_as<get_temp_path_w_fn>("kernel32.dll", "GetTempPathW");

    if (fn != nullptr && fn(cap, dst) != 0)
        return true;

    char a[512];
    if (GetTempPathA((DWORD)sizeof a, a) == 0)
        return false;
    if (MultiByteToWideChar(CP_ACP, 0, a, -1, dst, (int)cap) == 0)
        return false;
    return true;
}

/* 不一致时写：先打开成功、再写、再 SetEndOfFile（旧内容可能比 blob 长）。
 * 打开失败 → 明确报错，**不动**已有内容（不清写、不静默回退到内存映射）。 */
int write_over_w(const wchar_t *path, const unsigned char *blob, unsigned size)
{
    HANDLE w = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (w == INVALID_HANDLE_VALUE)
    {
        report_err("existing file differs but cannot be written (locked or read-only): not overwritten",
                   path, (unsigned)GetLastError());
        return -1;
    }

    DWORD written = 0;
    if (WriteFile(w, blob, size, &written, NULL) == 0 || written != size)
    {
        report_err("write failed", path, (unsigned)GetLastError());
        CloseHandle(w);
        return -1;
    }
    if (SetEndOfFile(w) == 0)
    {
        report_err("SetEndOfFile failed", path, (unsigned)GetLastError());
        CloseHandle(w);
        return -1;
    }
    CloseHandle(w);
    return 1;
}

/* 落盘：返回 0 = 内容一致（一个字节都没写、mtime 不变）、1 = 写了 / 新建、-1 = 明确报错 */
int ensure_blob(const wchar_t *path, const unsigned char *blob, unsigned size)
{
    /* 同一 build 串跨进程只读共享（已定：不加锁、不设 marker 文件），所以这里靠「读 → 逐字节比」
     * 而不是靠 marker；比完就关句柄。 */
    for (int attempt = 0; attempt < 2; attempt++)
    {
        HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE)
        {
            unsigned char *buf = (unsigned char *)autodbg::common::alloc(size + 1);
            DWORD          read = 0;
            bool           ok   = (ReadFile(h, buf, size, &read, NULL) != 0);
            unsigned       read_err = ok ? 0u : (unsigned)GetLastError();   /* ❗CloseHandle 会洗掉 last error */
            DWORD          extra = 0;   /* ❗LPDWORD：不能用 unsigned int（编译阶段就拒） */

            if (ok && read == size)
            {
                unsigned char probe = 0;
                if (ReadFile(h, &probe, 1, &extra, NULL) != 0 && extra == 1)
                    extra = 1;   /* 比 blob 长 → 内容不一致 */
                else
                    extra = 0;
            }
            CloseHandle(h);

            if (!ok)
            {
                report_err("cannot read existing file", path, read_err);
                autodbg::common::release(buf);
                return -1;
            }

            bool same = (read == size) && (extra == 0);
            for (unsigned i = 0; same && i < size; i++)
            {
                if (buf[i] != blob[i])
                    same = false;
            }
            autodbg::common::release(buf);

            if (same)
                return 0;
            return write_over_w(path, blob, size);
        }

        DWORD e = (unsigned)GetLastError();
        if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND)
        {
            report_err("cannot open target (read-only dir or no permission)", path, e);
            return -1;
        }

        if (attempt == 1)
        {
            /* 连两次都读不到：目录不可写 / 无权限 → 明确报错（plan §6 的 handoff.ro-media） */
            report_err("target not creatable (read-only or no-permission dir)", path, e);
            return -1;
        }

        HANDLE w = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
                               CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (w == INVALID_HANDLE_VALUE)
        {
            /* 竞态：另一个主 exe 实例刚建好同一份字节（已定：跨进程只读共享）→ 回到上面再比一次 */
            if ((unsigned)GetLastError() == ERROR_FILE_EXISTS)
                continue;
            report_err("cannot create target (read-only or no-permission dir)", path, (unsigned)GetLastError());
            return -1;
        }

        DWORD written = 0;
        if (WriteFile(w, blob, size, &written, NULL) == 0 || written != size)
        {
            report_err("write failed", path, (unsigned)GetLastError());
            CloseHandle(w);
            return -1;
        }
        CloseHandle(w);
        return 1;
    }

    report_err("no conclusion from the byte comparison", path, 0);
    return -1;
}

/* env `AUTODBG_BASE_DIR`（已定走 env，不占 boot 字段）= 原始主 exe 目录，**不带尾分隔符**
 *（已定：拼接时由 `common/base_dir` 补）。
 * `GetModuleFileName(NULL)` 在**主 exe** 里就是主 exe（§11.5 第 5 条是想要的语义）；child 里它变成
 * tmpdir 那份 ⇒ 脚本相对路径基准会变 ⇒ 必须显式把原始目录传过去。
 * 目录部分自己切（不用 `PathFindFileNameA`：它在 shlwapi，会新增导入库）。
 * best-effort：设不上只报一行，不因此失败（child 仍能跑，只是相对路径基准回到 CWD）。 */
void set_base_dir_env()
{
    char exe[512];
    if (GetModuleFileNameA(NULL, exe, (DWORD)sizeof exe) == 0)
    {
        report_err("base dir: GetModuleFileNameA failed", nullptr, (unsigned)GetLastError());
        return;
    }

    unsigned cut = 0;
    for (unsigned i = 0; exe[i] != '\0'; i++)
    {
        if (exe[i] == '\\' || exe[i] == '/')
            cut = i;   /* 分隔符本身不要：env 值不带尾分隔符（已定） */
    }
    if (cut == 0)
    {
        report_err("base dir: command line has no directory part", nullptr, 0);
        return;
    }
    exe[cut] = '\0';

    if (SetEnvironmentVariableA("AUTODBG_BASE_DIR", exe) == 0)
        report_err("env AUTODBG_BASE_DIR not set", nullptr, (unsigned)GetLastError());
}

/* job（三条全 dynapi，只有 wxp 有）：best-effort。
 * 嵌套 job（主 exe 已经在某个 job 里）assign 会失败 ⇒ **干净降级**：继续 wait、不报错、不退 2。
 * 不设任何 limit，只有 KILL_ON_JOB_CLOSE（它只覆盖「主 exe 被强杀」这一种）。 */
bool try_assign_job(HANDLE job, HANDLE process)
{
    set_information_job_object_fn sj = get_proc_as<set_information_job_object_fn>("kernel32.dll", "SetInformationJobObject");
    assign_process_to_job_object_fn aj = get_proc_as<assign_process_to_job_object_fn>("kernel32.dll", "AssignProcessToJobObject");

    if (sj == nullptr || aj == nullptr)
        return false;

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim;
    memset(&lim, 0, sizeof lim);
    /* 成员名：`BasicLimitInformation` + `LimitFlags`（`JobObjectLimitFlags` 是另一张结构
     * `JOBOBJECT_BASIC_JOB_LIMIT_INFORMATION` 的字段，用错会编译失败） */
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

    /* JobObjectExtendedLimitInformation（JO009）：用 SDK 的枚举值，不硬写 9 */
    if (sj(job, (int)JobObjectExtendedLimitInformation, &lim, (DWORD)sizeof lim) == 0)
        return false;
    if (aj(job, process) == 0)
        return false;   /* 嵌套 job：降级，不报错 */
    return true;
}

} /* namespace（匿名） */

int probe_host_is_64()
{
    is_wow64_process_fn fn = get_proc_as<is_wow64_process_fn>("kernel32.dll", "IsWow64Process");

    if (fn == nullptr)
        return 0;   /* 取不到 = 不是 64（95/98 本来没有 64 位） */

    BOOL wow = FALSE;
    if (fn(GetCurrentProcess(), &wow) == 0)
        return 0;
    return wow ? 1 : 0;
}

const char *host_arch_name()
{
    return probe_host_is_64() ? "x64" : "x86";
}

int run_handoff(const handoff_request &req)
{
    /* 错误出口只有一个（report_err）：弹窗与否由 `--quiet` 决定（与 launcher 的口径一致，已定）。
     * 文件作用域的 bool（POD 静态），在入口置位 —— 之后所有诊断都走同一个包装。 */
    s_quiet = req.quiet;

    if (req.blob_name == nullptr || req.blob_data == nullptr || req.blob_size == 0)
    {
        report_err("invalid request (no exec entry)", nullptr, 0);
        return 2;
    }

    /* ---- ① 落盘：tmpdir + manifest 里 exec 条目的名字（`adbg64.bin`），逐字节校验 ---- */
    wchar_t tmp[512];
    if (!get_temp_path_w(tmp, (unsigned)(sizeof tmp / sizeof tmp[0])))
    {
        report_err("GetTempPath failed", nullptr, (unsigned)GetLastError());
        return 2;
    }

    static wchar_t path[1024];
    unsigned       pos = 0;
    path[0] = 0;
    if (!append_w(path, (unsigned)(sizeof path / sizeof path[0]), &pos, tmp))
    {
        report_err("tmpdir path too long", nullptr, 0);
        return 2;
    }
    if (!append_cstr_w(path, (unsigned)(sizeof path / sizeof path[0]), &pos, req.blob_name))
    {
        report_err("blob name too long", path, 0);
        return 2;
    }

    int st = ensure_blob(path, req.blob_data, req.blob_size);
    if (st < 0)
        return 2;

    /* ---- ② base dir（env，不占 boot 字段）---- */
    set_base_dir_env();

    /* ---- ③ 拼 child 的 command line（引号口径 = autodbg_split_cmdline_a/w）----
     * argv[0] = child 自己那份落盘路径；其余逐字节原样。原命令优先用 W（非 ASCII 参数只有靠它才
     * 不被代码页截断），取不到 W 才用 A（9x 口径）。 */
    static wchar_t cmdline[k_cmdline_cap];
    const wchar_t *src = req.cmdline_w;
    static wchar_t src_a[1024];

    if (src == nullptr)
    {
        if (req.cmdline_a == nullptr)
        {
            report_err("no command line", nullptr, 0);
            return 2;
        }
        if (MultiByteToWideChar(CP_ACP, 0, req.cmdline_a, -1, src_a, (int)(sizeof src_a / sizeof src_a[0])) == 0)
        {
            report_err("command line convert failed", nullptr, (unsigned)GetLastError());
            return 2;
        }
        src = src_a;
    }

    unsigned cpos = 0;
    cmdline[0] = 0;
    if (!append_quoted_w(cmdline, k_cmdline_cap, &cpos, path))
    {
        report_err("child command line too long", path, 0);
        return 2;
    }

    const wchar_t *rest = after_first_token(src);
    if (*rest != 0)
    {
        if (*rest != L' ' && *rest != L'\t' && !append_w1(cmdline, k_cmdline_cap, &cpos, L' '))
        {
            report_err("child command line too long", path, 0);
            return 2;
        }
        if (!append_w(cmdline, k_cmdline_cap, &cpos, rest))
        {
            report_err("child command line too long", path, 0);
            return 2;
        }
    }

    if (req.append_script != nullptr)
    {
        static wchar_t script[1024];
        if (MultiByteToWideChar(CP_ACP, 0, req.append_script, -1, script, (int)(sizeof script / sizeof script[0])) == 0)
        {
            report_err("script path convert failed", nullptr, (unsigned)GetLastError());
            return 2;
        }
        if (!append_w1(cmdline, k_cmdline_cap, &cpos, L' ') || !append_quoted_w(cmdline, k_cmdline_cap, &cpos, script))
        {
            report_err("child command line too long", path, 0);
            return 2;
        }
    }

    /* ---- ④ job（先建，CreateProcess 后 assign）---- */
    create_job_object_w_fn cj = get_proc_as<create_job_object_w_fn>("kernel32.dll", "CreateJobObjectW");
    HANDLE job = (cj != nullptr) ? cj(nullptr, nullptr) : nullptr;

    PROCESS_INFORMATION pi;
    STARTUPINFOW        si;
    memset(&pi, 0, sizeof pi);
    memset(&si, 0, sizeof si);
    si.cb = (DWORD)sizeof si;

    create_process_w_fn cpw = get_proc_as<create_process_w_fn>("kernel32.dll", "CreateProcessW");
    create_process_a_fn cpa = get_proc_as<create_process_a_fn>("kernel32.dll", "CreateProcessA");

    bool spawned = false;
    if (cpw != nullptr)
    {
        /* ❗CreateProcess 不看扩展名：它按 PE 头判（`handoff.non-exe-name`）。名字来自 manifest，
         *   扩展名是 `.bin`（已定用 `adbg64.bin`）。 */
        spawned = (cpw(nullptr, cmdline, nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi) != 0);
    }
    else if (cpa != nullptr)
    {
        /* 9x 口径的降级：把已经拼好的宽命令行按 CP_ACP 转回窄串。handoff 在 9x 上实际到不了
         * 这一支（9x 不可能是 64 位 host），保留是为了「W 取不到也不崩」。 */
        static char cmdline_a[k_cmdline_cap];
        if (WideCharToMultiByte(CP_ACP, 0, cmdline, -1, cmdline_a, (int)sizeof cmdline_a, NULL, NULL) == 0)
        {
            report_err("command line convert back failed", path, (unsigned)GetLastError());
            if (job != nullptr)
                CloseHandle(job);
            return 2;
        }
        spawned = (cpa(nullptr, cmdline_a, nullptr, nullptr, TRUE, 0, nullptr, nullptr, NULL, &pi) != 0);
    }

    if (!spawned)
    {
        report_err("CreateProcess failed", path, (unsigned)GetLastError());
        if (job != nullptr)
            CloseHandle(job);
        return 2;
    }

    /* 取不到 / 嵌套失败 / limit 失败都算降级：继续 wait，不报错、不退 2。
     * job 句柄**不**因此关闭（KILL_ON_JOB_CLOSE 的语义是「句柄关闭时杀」，句柄归本函数管）。 */
    if (job != nullptr)
        try_assign_job(job, pi.hProcess);

    /* ---- ⑤ 退出码：口径不变（0 / -1 / 2 / Lua 错误码；-1 走 0xFFFFFFFF）---- */
    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD code = 0;
    if (GetExitCodeProcess(pi.hProcess, &code) == 0)
    {
        report_err("GetExitCodeProcess failed", nullptr, (unsigned)GetLastError());
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        if (job != nullptr)
            CloseHandle(job);
        return 2;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (job != nullptr)
        CloseHandle(job);   /* job 句柄在这里关闭 → KILL_ON_JOB_CLOSE 已经用不上（child 已退出） */

    return (int)code;
}

} /* namespace autodbg::sys::pack */
