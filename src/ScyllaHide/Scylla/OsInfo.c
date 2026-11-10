#include "OsInfo.h"
#include "Peb.h"

/**
 * Operating system                Version number
 * Windows 10                      10.0*
 * Windows Server 2016             10.0*
 * Windows 8.1                     6.3*
 * Windows Server 2012 R2          6.3*
 * Windows 8                       6.2
 * Windows Server 2012             6.2
 * Windows 7                       6.1
 * Windows Server 2008 R2          6.1
 * Windows Server 2008             6.0
 * Windows Vista                   6.0
 * Windows Server 2003 R2          5.2
 * Windows Server 2003             5.2
 * Windows XP 64-Bit Edition       5.2
 * Windows XP                      5.1
 * Windows 2000                    5.0
 */

const SYSTEM_INFO *scl_get_native_system_info(void)
{
    static SYSTEM_INFO si = { 0 };
    static BOOL cached = FALSE;

    if (!cached)
    {
        GetNativeSystemInfo(&si);
        cached = TRUE;
    }

    return &si;
}

const RTL_OSVERSIONINFOEXW *scl_get_version_ex_w(void)
{
    static RTL_OSVERSIONINFOEXW osVerInfo = { 0 };
    static BOOL cached = FALSE;

    if (!cached)
    {
        SCL_PEB *peb;

        osVerInfo.dwOSVersionInfoSize = sizeof(osVerInfo);
        RtlGetVersion((PRTL_OSVERSIONINFOW)&osVerInfo);
        peb = scl_get_peb_address(GetCurrentProcess());
        if (peb)
        {
            osVerInfo.dwMajorVersion = peb->OSMajorVersion;
            osVerInfo.dwMinorVersion = peb->OSMinorVersion;
        }
        cached = TRUE;
    }

    return &osVerInfo;
}

BOOL scl_is_windows_64(void)
{
#ifdef _WIN64
    return TRUE;
#else
    return (scl_get_native_system_info()->wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64);
#endif
}

BOOL scl_is_wow64_process(HANDLE hProcess)
{
    BOOL fIsWow64 = FALSE;
    return IsWow64Process(hProcess, &fIsWow64) && (fIsWow64 == TRUE);
}

scl_os_version scl_get_windows_version(void)
{
    static scl_os_version version = SCL_OS_UNKNOWN;
    const RTL_OSVERSIONINFOEXW *osVerInfo;

    if (version != SCL_OS_UNKNOWN)
        return version;

    version = SCL_OS_INVALID;

    osVerInfo = scl_get_version_ex_w();

    if (osVerInfo->dwMajorVersion == 5)
    {
        if (osVerInfo->dwMinorVersion == 0)
        {
            version = SCL_OS_WIN_2000;
        }
        else if (osVerInfo->dwMinorVersion == 1)
        {
            version = SCL_OS_WIN_XP;
        }
        else if (osVerInfo->dwMinorVersion == 2)
        {
            version = SCL_OS_WIN_XP64;
        }
    }
    else if (osVerInfo->dwMajorVersion == 6)
    {
        if (osVerInfo->dwMinorVersion == 0)
        {
            version = SCL_OS_WIN_VISTA;
        }
        else if (osVerInfo->dwMinorVersion == 1)
        {
            version = SCL_OS_WIN_7;
        }
        else if (osVerInfo->dwMinorVersion == 2)
        {
            version = SCL_OS_WIN_8;
        }
        else if (osVerInfo->dwMinorVersion == 3)
        {
            version = SCL_OS_WIN_81;
        }
    }
    else if (osVerInfo->dwMajorVersion == 10)
    {
        if (osVerInfo->dwMinorVersion == 0)
        {
            version = SCL_OS_WIN_10;
        }
    }

    return version;
}

const char *scl_get_windows_version_name_a(void)
{
    switch (scl_get_windows_version())
    {
    case SCL_OS_WIN_XP:
        return "Windows XP";
    case SCL_OS_WIN_XP64:
        return "Windows XP 64 / Server 2003";
    case SCL_OS_WIN_VISTA:
        return "Windows Vista / Server 2008";
    case SCL_OS_WIN_7:
        return "Windows 7 / Server 2008 R2";
    case SCL_OS_WIN_8:
        return "Windows 8 / Server 2012";
    case SCL_OS_WIN_81:
        return "Windows 8.1 / Server 2012 R2";
    case SCL_OS_WIN_10:
        return "Windows 10 / Server 2016";
    default:
        return "Unknown Windows";
    }
}
