#include "Peb.h"
#include "OsInfo.h"
#include "Util.h"

SCL_PEB *scl_get_peb_address(HANDLE hProcess)
{
    PROCESS_BASIC_INFORMATION pbi;
    NTSTATUS status;

    ZeroMemory(&pbi, sizeof(pbi));
    status = NtQueryInformationProcess(hProcess, ProcessBasicInformation, &pbi, sizeof(pbi), NULL);

    return NT_SUCCESS(status) ? (SCL_PEB *)pbi.PebBaseAddress : NULL;
}

/**
 * Get PEB64 address of WOW64 process.
 */
PVOID64 scl_get_peb64_address(HANDLE hProcess)
{
#ifndef _WIN64
    SCL_PROCESS_BASIC_INFORMATION64 pbi;

    ZeroMemory(&pbi, sizeof(pbi));
    if (scl_wow64_query_information_process64(hProcess, ProcessBasicInformation, &pbi, sizeof(pbi), NULL))
        return (PVOID64)pbi.PebBaseAddress;
#else
    UNREFERENCED_PARAMETER(hProcess);
#endif

    return NULL;
}

BOOL scl_get_peb(HANDLE hProcess, SCL_PEB *outPeb)
{
    SCL_PEB *peb_addr = scl_get_peb_address(hProcess);
    if (!peb_addr)
        return FALSE;

    return ReadProcessMemory(hProcess, peb_addr, outPeb, sizeof(SCL_PEB), NULL) == TRUE;
}

/**
 * @remark Use only real process handles.
 */
BOOL scl_wow64_get_peb64(HANDLE hProcess, SCL_PEB64 *outPeb64)
{
#ifndef _WIN64
    PVOID64 peb64_addr = scl_get_peb64_address(hProcess);
    if (!peb64_addr)
        return FALSE;

    return scl_wow64_read_process_memory64(hProcess, peb64_addr, outPeb64, sizeof(SCL_PEB64), NULL) ? TRUE : FALSE;
#else
    UNREFERENCED_PARAMETER(hProcess);
    UNREFERENCED_PARAMETER(outPeb64);
    return FALSE;
#endif
}

BOOL scl_set_peb(HANDLE hProcess, const SCL_PEB *pPeb)
{
    SCL_PEB *peb_addr = scl_get_peb_address(hProcess);
    if (!peb_addr)
        return FALSE;

    return WriteProcessMemory(hProcess, peb_addr, pPeb, sizeof(*pPeb), NULL) == TRUE;
}

/**
 * @remark Use only real process handles.
 */
BOOL scl_wow64_set_peb64(HANDLE hProcess, const SCL_PEB64 *pPeb64)
{
#ifndef _WIN64
    PVOID64 peb64_addr = scl_get_peb64_address(hProcess);
    if (!peb64_addr)
        return FALSE;

    return NT_SUCCESS(scl_wow64_write_process_memory64(hProcess, peb64_addr, pPeb64, sizeof(*pPeb64), NULL));
#else
    UNREFERENCED_PARAMETER(hProcess);
    UNREFERENCED_PARAMETER(pPeb64);
    return FALSE;
#endif
}

PVOID64 scl_wow64_get_module_handle64(HANDLE hProcess, const wchar_t *moduleName)
{
    SCL_PEB64 Peb64;
    SCL_PEB_LDR_DATA64 LdrData64;
    PVOID64 DllBase = NULL;
    ULONG64 LastEntry;
    SCL_LDR_DATA_TABLE_ENTRY64 Head;

    if (!scl_wow64_get_peb64(hProcess, &Peb64))
        return NULL;

    if (!scl_wow64_read_process_memory64(hProcess, (PVOID64)Peb64.Ldr, &LdrData64, sizeof(LdrData64), NULL))
        return NULL;

    LastEntry = Peb64.Ldr + offsetof(SCL_PEB_LDR_DATA64, InLoadOrderModuleList);
    Head.InLoadOrderLinks.Flink = LdrData64.InLoadOrderModuleList.Flink;

    do
    {
        wchar_t *BaseDllName;

        if (!scl_wow64_read_process_memory64(hProcess, (PVOID64)Head.InLoadOrderLinks.Flink, &Head, sizeof(Head), NULL))
            break;

        BaseDllName = (wchar_t *)RtlAllocateHeap(RtlProcessHeap(), HEAP_ZERO_MEMORY, Head.BaseDllName.MaximumLength);
        if (BaseDllName == NULL ||
            !scl_wow64_read_process_memory64(hProcess, (PVOID64)Head.BaseDllName.Buffer, BaseDllName, Head.BaseDllName.MaximumLength, NULL))
            break;

        if (_wcsicmp(moduleName, BaseDllName) == 0)
        {
            DllBase = (PVOID64)Head.DllBase;
        }

        RtlFreeHeap(RtlProcessHeap(), 0, BaseDllName);

    } while (Head.InLoadOrderLinks.Flink != LastEntry && DllBase == NULL);

    return DllBase;
}

DWORD scl_get_heap_flags_offset(BOOL x64)
{
    if (x64)
    {
        if (scl_get_windows_version() >= SCL_OS_WIN_VISTA)
            return 0x70;
        else
            return 0x14;
    }
    else
    {
        if (scl_get_windows_version() >= SCL_OS_WIN_VISTA)
            return 0x40;
        else
            return 0x0C;
    }
}

DWORD scl_get_heap_force_flags_offset(BOOL x64)
{
    if (x64)
    {
        if (scl_get_windows_version() >= SCL_OS_WIN_VISTA)
            return 0x74;
        else
            return 0x18;
    }
    else
    {
        if (scl_get_windows_version() >= SCL_OS_WIN_VISTA)
            return 0x44;
        else
            return 0x10;
    }
}
