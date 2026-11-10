#ifndef _STDLIB_H_
#define _STDLIB_H_

#ifdef __cplusplus
extern "C"
{
#endif

#ifdef _WIN64
    typedef unsigned long long size_t;
#else
typedef unsigned int size_t;
#endif

#ifndef _countof
#define _countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#ifndef NULL
#   ifdef __cplusplus
#       define NULL 0
#   else
#       define NULL ((void *)0)
#   endif
#endif

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

    __declspec(noreturn) void abort();
    __declspec(noreturn) void exit(int _Code);

#if !defined(_DEBUG) || !defined(_CRTDBG_MAP_ALLOC)
    void  free(void *_Block);
    void *malloc(size_t _Size);
    void *realloc(void *_Block, size_t _Size);
#endif

    char  *getenv(const char *_VarName);
    double strtod(const char *_String, char **_EndPtr);
    int    system(const char *_Command);

    unsigned long      strtoul(const char *_String, char **_EndPtr, int _Radix);
    /* 三架构统一由 src/sys/strtoull.c 提供：msvcrt.dll 只有 XP+ 的 _strtoui64 */
    unsigned long long strtoull(const char *_String, char **_EndPtr, int _Radix);
    int                atoi(const char *_String);
    long               strtol(const char *_String, char **_EndPtr, int _Radix);
    typedef int (*_CoreCrtNonSecureSearchSortCompareFunction)(const void *, const void *);
    void qsort(void *_Base, size_t _NumOfElements, size_t _SizeOfElements, _CoreCrtNonSecureSearchSortCompareFunction _CompareFunction);
    extern __declspec(dllimport) int _fmode;
#ifdef __cplusplus
}
#endif

#endif // _STDLIB_H_
