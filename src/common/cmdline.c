/* =============================================================================
 * src/common/cmdline.c —— Windows 命令行拆分（Plan §2.3 / §3.1）
 *
 * 来源：新写（Plan P3）。规则与 `CommandLineToArgvW` 一致（见 cmdline.h 头注释）。
 * 与旧实现的差异：无旧实现 —— 之前这两件事都由 msvcrt 的 `__getmainargs` 代劳。
 *
 * 为什么不用 `CommandLineToArgvW`：它在 shell32 里、要 `LocalFree`，且 Win95 没有
 *   （Win2000+ 才有）；本文件只用 char / wchar_t，零依赖。
 * ========================================================================== */
#include "cmdline.h"

#define AUTODBG_SPLIT_IMPL(CHAR) \
    do \
    { \
        CHAR *p = cmdline; \
        int   argc = 0; \
        \
        if (cmdline == NULL) \
            return 0; \
        \
        while (*p == ' ' || *p == '\t') \
            p++; \
        \
        /* --- argv[0]：特殊规则（引号内不做反斜杠转义） --- */ \
        if (argv != NULL && argc < max_argv) \
            argv[argc] = p; \
        argc++; \
        if (*p == '"') \
        { \
            p++; \
            while (*p != '\0' && *p != '"') \
                p++; \
            if (*p == '"') \
                *p++ = '\0'; \
        } \
        else \
        { \
            while (*p != '\0' && *p != ' ' && *p != '\t') \
                p++; \
            if (*p != '\0') \
                *p++ = '\0'; \
        } \
        \
        for (;;) \
        { \
            while (*p == ' ' || *p == '\t') \
                p++; \
            if (*p == '\0') \
                break; \
            if (argc >= max_argv) \
                return -1; \
            \
            { \
                CHAR *w = p; \
                int   quoted = 0; \
                \
                if (argv != NULL) \
                    argv[argc] = w; \
                argc++; \
                \
                for (;;) \
                { \
                    if (*p == '\0') \
                        break; \
                    if (*p == '\\') \
                    { \
                        int n = 0; \
                        while (*p == '\\') \
                        { \
                            n++; \
                            p++; \
                        } \
                        if (*p == '"') \
                        { \
                            for (int i = 0; i < n / 2; i++) \
                                *w++ = '\\'; \
                            if (n & 1) \
                            { \
                                *w++ = '"'; \
                                p++; \
                            } \
                            else \
                            { \
                                quoted = !quoted; \
                                p++; \
                            } \
                        } \
                        else \
                        { \
                            for (int i = 0; i < n; i++) \
                                *w++ = '\\'; \
                        } \
                    } \
                    else if (*p == '"') \
                    { \
                        quoted = !quoted; \
                        p++; \
                    } \
                    else if (!quoted && (*p == ' ' || *p == '\t')) \
                    { \
                        break; \
                    } \
                    else \
                    { \
                        *w++ = *p++; \
                    } \
                } \
                if (*p != '\0') \
                    p++; \
                *w = '\0'; \
            } \
        } \
        \
        if (argv != NULL) \
            argv[argc] = NULL; \
        return argc; \
    } while (0)

int autodbg_split_cmdline_a(char *cmdline, char **argv, int max_argv)
{
    AUTODBG_SPLIT_IMPL(char);
}

int autodbg_split_cmdline_w(wchar_t *cmdline, wchar_t **argv, int max_argv)
{
    AUTODBG_SPLIT_IMPL(wchar_t);
}
