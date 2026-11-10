#pragma once

#include <Windows.h>
#include "NtApiShim.h"

#ifdef __cplusplus
extern "C" {
#endif

BOOL scl_wow64_query_information_process64(HANDLE hProcess, PROCESSINFOCLASS ProcessInformationClass, PVOID ProcessInformation, ULONG ProcessInformationLength, PULONG ReturnLength);

BOOL scl_wow64_read_process_memory64(HANDLE hProcess, PVOID64 address, PVOID buffer, ULONGLONG buffer_size, PULONGLONG bytes_read);
BOOL scl_wow64_write_process_memory64(HANDLE hProcess, PVOID64 address, LPCVOID buffer, ULONGLONG buffer_size, PULONGLONG bytes_written);

#ifdef __cplusplus
}
#endif
