#pragma once

#include <ntdll/ntdll.h>

#ifndef FLG_HEAP_ENABLE_TAIL_CHECK
#define FLG_HEAP_ENABLE_TAIL_CHECK 0x10
#endif

#ifndef FLG_HEAP_ENABLE_FREE_CHECK
#define FLG_HEAP_ENABLE_FREE_CHECK 0x20
#endif

#ifndef FLG_HEAP_VALIDATE_PARAMETERS
#define FLG_HEAP_VALIDATE_PARAMETERS 0x40
#endif

#ifndef HEAP_SKIP_VALIDATION_CHECKS
#define HEAP_SKIP_VALIDATION_CHECKS 0x10000000
#endif

#ifndef HEAP_VALIDATE_PARAMETERS_ENABLED
#define HEAP_VALIDATE_PARAMETERS_ENABLED 0x40000000
#endif

#ifndef DBG_PRINTEXCEPTION_WIDE_C
#define DBG_PRINTEXCEPTION_WIDE_C ((DWORD)0x4001000A)
#endif

typedef NTSTATUS(WINAPI *t_NtWow64QueryInformationProcess64)(HANDLE ProcessHandle, PROCESSINFOCLASS ProcessInformationClass, PVOID ProcessInformation, ULONG ProcessInformationLength, PULONG ReturnLength);
typedef NTSTATUS(WINAPI *t_NtWow64ReadVirtualMemory64)(HANDLE ProcessHandle, PVOID64 BaseAddress, PVOID Buffer, ULONGLONG BufferSize, PULONGLONG NumberOfBytesRead);
typedef NTSTATUS(WINAPI *t_NtWow64WriteVirtualMemory64)(HANDLE ProcessHandle, PVOID64 BaseAddress, LPCVOID Buffer, ULONGLONG BufferSize, PULONGLONG NumberOfBytesWritten);

// Instantiate the pointer-width dependent structures for both bitnesses.
// The 32-bit variant is used by the native process, the 64-bit one for WOW64 targets.
// All types carry the SCL_ prefix because C has no namespace and the SDK/phnt define
// generic names such as UNICODE_STRING, PROCESS_BASIC_INFORMATION and LIST_ENTRY32.

#define SCL_PTR DWORD
#define SCL_UNICODE_STRING SCL_UNICODE_STRING32
#define SCL_CURDIR SCL_CURDIR32
#define SCL_PROCESS_BASIC_INFORMATION SCL_PROCESS_BASIC_INFORMATION32
#define SCL_RTL_USER_PROCESS_PARAMETERS SCL_RTL_USER_PROCESS_PARAMETERS32
#include "NtApiShim.inl"
#undef SCL_PTR
#undef SCL_UNICODE_STRING
#undef SCL_CURDIR
#undef SCL_PROCESS_BASIC_INFORMATION
#undef SCL_RTL_USER_PROCESS_PARAMETERS

#define SCL_PTR DWORD64
#define SCL_UNICODE_STRING SCL_UNICODE_STRING64
#define SCL_CURDIR SCL_CURDIR64
#define SCL_PROCESS_BASIC_INFORMATION SCL_PROCESS_BASIC_INFORMATION64
#define SCL_RTL_USER_PROCESS_PARAMETERS SCL_RTL_USER_PROCESS_PARAMETERS64
#include "NtApiShim.inl"
#undef SCL_PTR
#undef SCL_UNICODE_STRING
#undef SCL_CURDIR
#undef SCL_PROCESS_BASIC_INFORMATION
#undef SCL_RTL_USER_PROCESS_PARAMETERS

// Pointer-width dependent typedef matching the current process (the old DWORD_PTR<...> instantiation).
#ifdef _WIN64
typedef SCL_RTL_USER_PROCESS_PARAMETERS64 SCL_RTL_USER_PROCESS_PARAMETERS_NATIVE;
#else
typedef SCL_RTL_USER_PROCESS_PARAMETERS32 SCL_RTL_USER_PROCESS_PARAMETERS_NATIVE;
#endif
