/* =============================================================================
 * src/sys/pack/pack_entry.c —— 启动器的进程入口（Plan §2.3 / F10）
 *
 * 职责：`mainCRTStartup` → `GetCommandLineA` → `autodbg_split_cmdline_a` → `launcher_main()` → `ExitProcess`。
 * 来源：新写（Plan P3）。与 `src/sys/main.c`（exe 用的入口）的差异：
 *   ① **不调 `__getmainargs`**（plan §0 收益 4：Win9x 上的风险项，顺手去掉这个 msvcrt 依赖）；
 *   ② 命令行先拷进本文件自己的静态缓冲再原地拆分（`GetCommandLineA` 的串属于 PEB，不写它）；
 *   ③ 不设 `_CrtMemState`（启动器没有 CRT 调试设施）。
 *
 * ❗C 链接：`/ENTRY:mainCRTStartup` 按 C 名字查找。
 * ========================================================================== */
#include <windows.h>

/* src/common/cmdline.h 声明的两个函数 */
int autodbg_split_cmdline_a(char *cmdline, char **argv, int max_argv);

/* 启动器主体（launcher.cpp） */
int launcher_main(int argc, char **argv);

/* 命令行上限：Win32 的 CreateProcess 限制是 32767 字符（含 NUL 共 32768） */
static char s_cmdline[32768];
static char *s_argv[128];

void mainCRTStartup(void)
{
    const char *src = GetCommandLineA();
    unsigned    i   = 0;

    if (src != NULL)
    {
        for (; src[i] != '\0' && i + 1 < sizeof s_cmdline; i++)
            s_cmdline[i] = src[i];
    }
    s_cmdline[i] = '\0';

    int argc = autodbg_split_cmdline_a(s_cmdline, s_argv, (int)(sizeof s_argv / sizeof s_argv[0]));
    if (argc < 0)
        argc = 0;

    ExitProcess((UINT)launcher_main(argc, s_argv));
}

/* 浮点支持标记：与 src/sys/main.c 同口径（零 CRT 下无害，缺了会让某些 /O2 代码报 LNK2019） */
long _fltused = 1;
