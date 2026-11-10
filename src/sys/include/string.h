#ifndef _STRING_H_
#define _STRING_H_

#ifdef __cplusplus
extern "C"
{
#endif

#ifndef __cplusplus
    /* C++ 有内建 wchar_t（同宽同语义）→ 仅 C 编译时定义。见 Plan.md P1.2。 */
    typedef unsigned short wchar_t;
#endif

#ifdef _UNICODE
#else
#endif

#ifndef NULL
#   ifdef __cplusplus
#       define NULL 0
#   else
#       define NULL ((void *)0)
#   endif
#endif

#ifdef _WIN64
    typedef unsigned long long size_t;
#else
typedef unsigned int size_t;
#endif

    typedef char                         *va_list;
    typedef struct __crt_locale_pointers *_locale_t;

    const void *memchr(const void *_Buf, int _Val, size_t _MaxCount);
    int         memcmp(const void *_Buf1, const void *_Buf2, size_t _Size);
    void       *memcpy(void *_Dst, const void *_Src, size_t _Size);
    void       *memmove(void *_Dst, const void *_Src, size_t _Size);
    void       *memset(void *_Dst, int _Val, size_t _Size);
    char       *strchr(const char *_Str, int _Val);
    char       *strrchr(char *const _String, const int _Ch);
    char       *_strdup(const char *_Source);

    char    *strstr(const char *_Str, const char *_SubStr);
    wchar_t *wcschr(const wchar_t *_Str, wchar_t _Ch);
    wchar_t *wcsrchr(const wchar_t *_Str, wchar_t _Ch);
    wchar_t *wcsstr(const wchar_t *_Str, const wchar_t *_SubStr);
    wchar_t *wcscpy(wchar_t *_Destination, const wchar_t *_Source);

    int    strcmp(const char *_Str1, const char *_Str2);
    int    _strcmpi(const char *_String1, const char *_String2);
    int    strcoll(const char *_String1, const char *_String2);
    int    _strcoll_l(const char *_String1, const char *_String2, _locale_t _Locale);
    size_t strcspn(const char *_Str, const char *_Control);
    int    strncmp(const char *_Str1, const char *_Str2, size_t _MaxCount);
    char  *strerror(int _ErrorMessage);
    size_t strlen(const char *_Str);
    size_t strspn(const char *_Str, const char *_Control);
    char  *strpbrk(const char *_String, const char *_Control);
    char  *strcpy(char *_Destination, const char *_Source);
    char  *strncpy(char *_Destination, const char *_Source, size_t _Count);
    char  *strcat(char *_Destination, const char *_Source);
    char  *strncat(char *_Destination, const char *_Source, size_t _Count);
    int    _wcsicmp(const wchar_t *_String1, const wchar_t *_String2);
    int    _stricmp(const char *_String1, const char *_String2);
    size_t wcslen(const wchar_t *_String);
    int    _snprintf(char *_Buffer, size_t _BufferCount, const char *_Format, ...);
    int    _snwprintf(wchar_t *_Buffer, size_t _BufferCount, const wchar_t *_Format, ...);
    int    _vsnprintf(char *_Buffer, size_t _BufferCount, const char *_Format, va_list _ArgList);
    int    _vsnwprintf(wchar_t *_Buffer, size_t _BufferCount, const wchar_t *_Format, va_list _ArgList);

#ifdef _UNICODE
#   define _tcslen  wcslen
#   define _tcsicmp _wcsicmp
#else
#   define _tcslen  strlen
#   define _tcsicmp _stricmp
#endif

#ifdef __cplusplus
}
#endif

#endif
