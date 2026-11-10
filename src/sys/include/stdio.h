#ifndef _STDIO_H_
#define _STDIO_H_

#ifdef __cplusplus
extern "C"
{
#endif
#define BUFSIZ 512
#define EOF    (-1)

#define SEEK_CUR 1
#define SEEK_END 2
#define SEEK_SET 0

#define _IOFBF   0x0000
#define _IOLBF   0x0040
#define _IONBF   0x0004
#define L_tmpnam 260 // _MAX_PATH

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
#ifndef __cplusplus
    /* C++ 有内建 wchar_t（Windows 上 2 字节，与 unsigned short 同宽同语义），
       在 C++ 下重定义会报错；仅 C 编译时定义。见 Plan.md P1.2。 */
    typedef unsigned short wchar_t;
#endif
    typedef unsigned short wint_t;
    typedef char          *va_list;

    struct _iobuf
    {
        char *_ptr;
        int   _cnt;
        char *_base;
        int   _flag;
        int   _file;
        int   _charbuf;
        int   _bufsiz;
        char *_tmpfname;
    };
    typedef struct _iobuf FILE;

#ifdef _WIN64
    FILE *__iob_func();

#   define stdin  (&__iob_func()[0])
#   define stdout (&__iob_func()[1])
#   define stderr (&__iob_func()[2])
#else
FILE *__p__iob();

#   define stdin  (&__p__iob()[0])
#   define stdout (&__p__iob()[1])
#   define stderr (&__p__iob()[2])
#endif

    FILE *fopen(const char *_FileName, const char *_Mode);
    int   fclose(FILE *_Stream);
    int   fprintf(FILE *const _Stream, const char *const _Format, ...);
    int   vfprintf(FILE *const _Stream, const char *const _Format, va_list _ArgList);
    int   vfwprintf(FILE *const _Stream, const wchar_t *const _Format, va_list _ArgList);
    int   fflush(FILE *_Stream);

    int    feof(FILE *_Stream);
    size_t fread(void *_Buffer, size_t _ElementSize, size_t _ElementCount, FILE *_Stream);
    int    getc(FILE *_Stream);
    FILE  *freopen(const char *_FileName, const char *_Mode, FILE *_Stream);
    int    ferror(FILE *_Stream);
    size_t fwrite(const void *_Buffer, size_t _ElementSize, size_t _ElementCount, FILE *_Stream);
    char  *fgets(char *_Buffer, int _MaxCount, FILE *_Stream);
    int    _pclose(FILE *_Stream);
    FILE  *_popen(const char *_Command, const char *_Mode);
    FILE  *tmpfile(void);
    int    ungetc(int _Character, FILE *_Stream);
    void   clearerr(FILE *_Stream);
    int    fseek(FILE *_Stream, long _Offset, int _Origin);
    long   ftell(FILE *_Stream);
    int    setvbuf(FILE *_Stream, char *_Buffer, int _Mode, size_t _Size);
    int    sprintf(char *const _Buffer, const char *const _Format, ...);
    int    remove(const char *_FileName);
    int    rename(const char *_OldFileName, const char *_NewFileName);
    char  *tmpnam(char *_Buffer);
    int    fputs(const char *_Buffer, FILE *_Stream);
    int    printf(const char *const _Format, ...);
    int    _fileno(FILE *_Stream);
    int    fscanf(FILE *const _Stream, const char *const _Format, ...);
    int    putc(int _Character, FILE *_Stream);
    int    fputc(int _Character, FILE *_Stream);
    int    putchar(int _Character);
    int    puts(const char *_Buffer);
    FILE  *_wfopen(const wchar_t *_FileName, const wchar_t *_Mode);
    int    _vsnprintf(char *const _Buffer, const size_t _BufferCount, const char *const _Format, va_list _ArgList);
    wint_t getwc(FILE *_Stream);
    int    _vscprintf(const char *const _Format, va_list _ArgList);
    int    _vscwprintf(const wchar_t *const _Format, va_list _ArgList);
    int    fwprintf(FILE *const _Stream, const wchar_t *const _Format, ...);
    int    sprintf_s(char *const _Buffer, const size_t _BufferCount, const char *const _Format, ...);
    int    _putws(const wchar_t *_Buffer);
    int    wprintf(const wchar_t *const _Format, ...);
#ifdef _WIN64
    int     _fseeki64(FILE *_Stream, __int64 _Offset, int _Origin);
    __int64 _ftelli64(FILE *_Stream);
#else
#   define _fseeki64 fseek
#   define _ftelli64 ftell
#endif

#ifdef _UNICODE
#   define _ftprintf  fwprintf
#   define _vftprintf vfwprintf
#   define _fputtc    fputwc
#   define _tfopen    _wfopen
#else
#   define _ftprintf  fprintf
#   define _vftprintf vfprintf
#   define _fputtc    fputc
#   define _tfopen    fopen
#endif

#ifdef __cplusplus
}
#endif

#endif // _STDIO_H_
