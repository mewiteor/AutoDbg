#ifndef _TIME_H_
#define _TIME_H_

#ifdef __cplusplus
extern "C"
{
#endif
#ifdef _WIN64
    typedef unsigned long long size_t;
#else
typedef unsigned int size_t;
#endif

    typedef long clock_t;
#ifdef _WIN64
    typedef __int64 __time64_t;
    typedef __time64_t time_t;
#else
typedef long time_t;
#endif
    struct timespec
    {
        time_t tv_sec; // Seconds - >= 0
        long tv_nsec;  // Nanoseconds - [0, 999999999]
    };
    struct tm
    {
        int tm_sec;   // seconds after the minute - [0, 60] including leap second
        int tm_min;   // minutes after the hour - [0, 59]
        int tm_hour;  // hours since midnight - [0, 23]
        int tm_mday;  // day of the month - [1, 31]
        int tm_mon;   // months since January - [0, 11]
        int tm_year;  // years since 1900
        int tm_wday;  // days since Sunday - [0, 6]
        int tm_yday;  // days since January 1 - [0, 365]
        int tm_isdst; // daylight savings time flag
    };
#define CLOCKS_PER_SEC ((clock_t)1000)
#define TIME_UTC 1
    clock_t clock();
#ifdef _WIN64
    __time64_t _time64(__time64_t *_Time);
    static __inline time_t time(time_t *const _Time)
    {
        return _time64(_Time);
    }
    __time64_t _mktime64(struct tm *_Tm);
    static __inline time_t mktime(struct tm *const _Tm)
    {
        return _mktime64(_Tm);
    }
    double _difftime64(__time64_t _Time1, __time64_t _Time2);
    static __inline double difftime(time_t const _Time1, time_t const _Time2)
    {
        return _difftime64(_Time1, _Time2);
    }
    struct tm *_gmtime64(__time64_t const *_Time);
    static __inline struct tm *gmtime(time_t const *const _Time)
    {
        return _gmtime64(_Time);
    }
    struct tm *_localtime64(__time64_t const *_Time);
    static __inline struct tm *localtime(time_t const *const _Time)
    {
        return _localtime64(_Time);
    }
#else
time_t time(time_t *const _Time);
time_t mktime(struct tm *const _Tm);
double difftime(time_t const _Time1, time_t const _Time2);
struct tm *gmtime(time_t const *const _Time);
struct tm *localtime(time_t const *const _Time);
#endif
    size_t strftime(char *_Buffer, size_t _SizeInBytes, char const *_Format, struct tm const *_Tm);

#ifdef __cplusplus
}
#endif

#endif // _TIME_H_
