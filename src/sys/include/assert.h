#ifndef _ASSERT_H_
#define _ASSERT_H_

#ifdef __cplusplus
extern "C"
{
#endif

    void _assert(const char *, const char *, unsigned);

#define assert(expression)                            \
    do                                                \
    {                                                 \
        if (!(expression))                            \
        {                                             \
            _assert(#expression, __FILE__, __LINE__); \
        }                                             \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif // _ASSERT_H_
