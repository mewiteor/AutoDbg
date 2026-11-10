#ifndef AUTODBG_COMMON_CMDLINE_H
#define AUTODBG_COMMON_CMDLINE_H
/* =============================================================================
 * src/common/cmdline.h —— 命令行拆分（启动器与模块 DLL 共用，Plan §2.3 / §3.1）
 *
 * 职责：把 `GetCommandLineA/W` 的原始字符串按 Windows 规则拆成 argv（**原地**拆分）。
 * 来源：新写（Plan P3）。替代 `__getmainargs` / `__wgetmainargs`：
 *   ① 那两个是 msvcrt 的实现，Win9x 上未必一致（plan §0 收益 4 提到这个风险项）；
 *   ② 启动器要用自己的 argv 决定装哪个模块，模块 DLL 要按 `boot.cmdline_*` 重新拆一次
 *      （映射后的模块不在 OS 加载器手里，拿不到「进程参数」）。
 *
 * 规则（与 `CommandLineToArgvW` / msvcrt 一致）：
 *   - argv[0] 用特殊规则：若以 `"` 开头，取到下一个 `"` 为止，**不做反斜杠转义**；
 *   - 后续参数：`2n` 个反斜杠 + `"` → n 个反斜杠 + 引号开关；
 *                 `2n+1` 个反斜杠 + `"` → n 个反斜杠 + 字面引号；
 *   - 引号外的空格 / 制表符是分隔符。
 *
 * ❗`buf` 会被就地改写（写入 NUL），调用方必须传可写缓冲。
 * ========================================================================== */
/* <string.h>（影子头）：需要 wchar_t。C 编译时 wchar_t 不是内置类型，
 * 影子 string.h 会在 !__cplusplus 下 typedef 它；stddef.h 没有这个。 */
#include <string.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* 拆分 ANSI 命令行：返回 argc（>= 1）；`argv` 为 NULL 时只数个数；超过 max_argv 返回 -1。
 * 成功时 `argv[argc]` 被置为 NULL（若 argv 非空且还有空间）。 */
int autodbg_split_cmdline_a(char *cmdline, char **argv, int max_argv);

/* 同上，宽字符版 */
int autodbg_split_cmdline_w(wchar_t *cmdline, wchar_t **argv, int max_argv);

#ifdef __cplusplus
}
#endif

#endif /* AUTODBG_COMMON_CMDLINE_H */
