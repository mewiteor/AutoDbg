/* =============================================================================
 * src/sys/strtoull.c —— 影子 stdlib.h 的 strtoull 实现（零 CRT）
 *
 * 文件职责
 *     为 thirdpart/luajit/src/lib_base.c 的 `tonumber(s, base)` 提供
 *     `strtoull`。零 CRT（/nodefaultlib）下不能链接 MSVC 的 CRT 目标文件，
 *     只能自己实现；本文件不引入任何导入表符号。
 *
 * 为什么不用现成的运行时符号
 *     · 64 位 ucrtbase.dll 导出 `strtoull`，但 32 位 msvcrt.dll 只导出
 *       `_strtoui64`（XP+），Win9x 的 msvcrt.dll 连它都没有。一旦静态导入，
 *       Win9x 上「加载就失败」（AGENTS §5.3：x86 产物导入表零 XP+）。
 *     · 所以三架构统一用本文件的实现，导入表零新增。
 *
 * 语义（对齐 C11 7.22.1.4 / MSVC CRT）
 *     · 跳过前导空白 → 可选 '+'/'-' → 按 base 消费数字；
 *     · base == 0 时按 0x/0x 前缀推断进制，base == 16 时允许 0x 前缀；
 *     · 没有可消费数字：返回 0，*endptr = nptr（表示「未转换」）；
 *     · 溢出：返回 ULLONG_MAX 并置 errno = ERANGE；负号按补码取负。
 *
 * 与旧实现的差异
 *     新增文件（LuaJIT 接入）；此前三个架构都没有 `strtoull` 的提供者，
 *     LuaJIT 的 lib_base.c 只能落到隐式声明（返回值按 int 截断，64 位上是错的）。
 * ========================================================================== */

#include <errno.h>

#define ULLONG_MAX_VALUE 0xFFFFFFFFFFFFFFFFull

/* 仅 ASCII 空白，与 CRT 的 isspace 在这些字符上一致，不依赖 locale */
static int shim_isspace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

/* 返回 0..35，非法字符返回 -1 */
static int shim_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z')
        return c - 'A' + 10;
    return -1;
}

unsigned long long strtoull(const char *nptr, char **endptr, int base)
{
    const char *p = nptr;
    unsigned long long acc = 0;
    int neg = 0;
    int any = 0;
    int overflow = 0;

    while (shim_isspace(*p))
        p++;

    if (*p == '+' || *p == '-')
    {
        neg = (*p == '-');
        p++;
    }

    if (base == 0)
    {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        {
            base = 16;
            p += 2;
        }
        else if (p[0] == '0')
        {
            base = 8;
        }
        else
        {
            base = 10;
        }
    }
    else if (base == 16)
    {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
            p += 2;
    }

    if (base < 2 || base > 36)
        return 0;

    for (;;)
    {
        int digit = shim_digit(*p);
        if (digit < 0 || digit >= base)
            break;
        any = 1;
        if (acc > (ULLONG_MAX_VALUE - (unsigned long long)digit) / (unsigned long long)base)
            overflow = 1;
        else
            acc = acc * (unsigned long long)base + (unsigned long long)digit;
        p++;
    }

    if (!any)
        return 0; /* 未转换：endptr 保持 nptr */

    if (overflow)
    {
        acc = ULLONG_MAX_VALUE;
        errno = ERANGE;
    }
    else if (neg)
    {
        acc = ~acc + 1ull;
    }

    if (endptr)
        *endptr = (char *)p;

    return acc;
}
