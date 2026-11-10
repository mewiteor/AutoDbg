#ifndef _IO_H_
#define _IO_H_

#ifdef __cplusplus
extern "C"
{
#endif

#ifdef _WIN64
    typedef unsigned long long size_t;
#else
typedef unsigned int size_t;
#endif

    int _isatty(int _FileHandle);
    int _setmode(int _FileHandle, int _Mode);

#ifdef __cplusplus
}
#endif

#endif // _IO_H_
