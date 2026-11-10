#include "Util.h"
#include "NtApiShim.h"

BOOL scl_wow64_query_information_process64(HANDLE hProcess, PROCESSINFOCLASS ProcessInformationClass, PVOID ProcessInformation, ULONG ProcessInformationLength, PULONG ReturnLength)
{
    t_NtWow64QueryInformationProcess64 _NtWow64QueryInformationProcess64 = (t_NtWow64QueryInformationProcess64)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtWow64QueryInformationProcess64");
    if (!_NtWow64QueryInformationProcess64)
        return FALSE;

    return NT_SUCCESS(_NtWow64QueryInformationProcess64(hProcess, ProcessInformationClass, ProcessInformation, ProcessInformationLength, ReturnLength));
}

BOOL scl_wow64_read_process_memory64(HANDLE hProcess, PVOID64 address, PVOID buffer, ULONGLONG buffer_size, PULONGLONG bytes_read)
{
#ifndef _WIN64
    t_NtWow64ReadVirtualMemory64 _NtWow64ReadVirtualMemory64 = (t_NtWow64ReadVirtualMemory64)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtWow64ReadVirtualMemory64");
    if (_NtWow64ReadVirtualMemory64)
    {
        return NT_SUCCESS(_NtWow64ReadVirtualMemory64(hProcess, address, buffer, buffer_size, bytes_read));
    }
    else if ((((DWORD64)address + buffer_size) < (DWORD)(-1)) && (buffer_size <= (DWORD)(-1)))
    {
        SIZE_T bytes_read32 = 0;
        BOOL ret = ReadProcessMemory(hProcess, (PVOID)(ULONG)(ULONG64)address, buffer, (SIZE_T)buffer_size, &bytes_read32);
        if (bytes_read)
            *bytes_read = bytes_read32;
        return ret;
    }
#endif

    return FALSE;
}

BOOL scl_wow64_write_process_memory64(HANDLE hProcess, PVOID64 address, LPCVOID buffer, ULONGLONG buffer_size, PULONGLONG bytes_written)
{
#ifndef _WIN64
    t_NtWow64WriteVirtualMemory64 _NtWow64WriteVirtualMemory64 = (t_NtWow64WriteVirtualMemory64)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtWow64WriteVirtualMemory64");
    if (_NtWow64WriteVirtualMemory64)
    {
        return NT_SUCCESS(_NtWow64WriteVirtualMemory64(hProcess, address, buffer, buffer_size, bytes_written));
    }
    else if ((((DWORD64)address + buffer_size) < (DWORD)(-1)) && (buffer_size <= (DWORD)(-1)))
    {
        SIZE_T bytes_written32 = 0;
        BOOL ret = WriteProcessMemory(hProcess, (PVOID)(ULONG)(ULONG64)(address), buffer, (SIZE_T)buffer_size, &bytes_written32);
        if (bytes_written)
            *bytes_written = bytes_written32;
        return ret;
    }
#endif

    return FALSE;
}
