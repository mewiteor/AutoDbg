#pragma once

#include <Windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum scl_os_version
{
    SCL_OS_UNKNOWN = 0,
    SCL_OS_INVALID,
    SCL_OS_WIN_2000,
    SCL_OS_WIN_XP,
    SCL_OS_WIN_XP64,
    SCL_OS_WIN_VISTA,
    SCL_OS_WIN_7,
    SCL_OS_WIN_8,
    SCL_OS_WIN_81,
    SCL_OS_WIN_10
} scl_os_version;

const SYSTEM_INFO *scl_get_native_system_info(void);
const RTL_OSVERSIONINFOEXW *scl_get_version_ex_w(void);

BOOL scl_is_windows_64(void);
BOOL scl_is_wow64_process(HANDLE hProcess);
scl_os_version scl_get_windows_version(void);
const char *scl_get_windows_version_name_a(void);

#ifdef __cplusplus
}
#endif
