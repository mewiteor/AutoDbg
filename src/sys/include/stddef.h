#ifndef _STDDEF_H_
#define _STDDEF_H_

#ifdef __cplusplus
extern "C"
{
#endif

    /* 影子头里原本只有 NULL/errno：`tools/PackEXE/pe/lzma` 迁入后（P1）需要这两个类型 ——
     *   size_t   ：7zTypes.h 只 include <stddef.h>（不经过 string.h/stdlib.h）
     *   ptrdiff_t：LzFind.c（编码侧）直接用
     * 与 string.h / stdlib.h 里的 size_t 定义一致（C11 允许重复 typedef，实测通过）。 */
#ifdef _WIN64
    typedef unsigned long long size_t;
    typedef long long          ptrdiff_t;
#else
    typedef unsigned int       size_t;
    typedef int                ptrdiff_t;
#endif

#ifndef NULL
#   ifdef __cplusplus
#       define NULL 0
#   else
#       define NULL ((void *)0)
#   endif
#endif

    int *_errno();
#define errno (*_errno())

    typedef int errno_t;
    errno_t _set_errno(int _Value);
    errno_t _get_errno(int *_Value);
#define offsetof(s,m) ((size_t)&(((s*)0)->m))

#ifdef __cplusplus
}
#endif

#endif // _STDDEF_H_

