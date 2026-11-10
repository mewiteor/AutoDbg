#pragma once

#include <stddef.h>
#include "NtApiShim.h"

//
// http://terminus.rewolf.pl/terminus/structures/ntdll/_PEB_combined.html
//

// Instantiate the PEB structure family for both bitnesses.
// Only the macro values change per bitness; the struct bodies live in PebStructs.inl.

#define SCL_PTR DWORD
#define SCL_NGF DWORD64
#define SCL_GDI_HANDLE_BUFFER_LEN 34
#define SCL_UNICODE_STRING SCL_UNICODE_STRING32
#define SCL_LIST_ENTRY SCL_LIST_ENTRY32
#define SCL_LDR_UNICODE_STRING SCL_LDR_UNICODE_STRING32
#define SCL_LDR_DATA_TABLE_ENTRY SCL_LDR_DATA_TABLE_ENTRY32
#define SCL_PEB_LDR_DATA SCL_PEB_LDR_DATA32
#define SCL_PEB SCL_PEB32
#include "PebStructs.inl"
#undef SCL_PTR
#undef SCL_NGF
#undef SCL_GDI_HANDLE_BUFFER_LEN
#undef SCL_UNICODE_STRING
#undef SCL_LIST_ENTRY
#undef SCL_LDR_UNICODE_STRING
#undef SCL_LDR_DATA_TABLE_ENTRY
#undef SCL_PEB_LDR_DATA
#undef SCL_PEB

#define SCL_PTR DWORD64
#define SCL_NGF DWORD
#define SCL_GDI_HANDLE_BUFFER_LEN 30
#define SCL_UNICODE_STRING SCL_UNICODE_STRING64
#define SCL_LIST_ENTRY SCL_LIST_ENTRY64
#define SCL_LDR_UNICODE_STRING SCL_LDR_UNICODE_STRING64
#define SCL_LDR_DATA_TABLE_ENTRY SCL_LDR_DATA_TABLE_ENTRY64
#define SCL_PEB_LDR_DATA SCL_PEB_LDR_DATA64
#define SCL_PEB SCL_PEB64
#include "PebStructs.inl"
#undef SCL_PTR
#undef SCL_NGF
#undef SCL_GDI_HANDLE_BUFFER_LEN
#undef SCL_UNICODE_STRING
#undef SCL_LIST_ENTRY
#undef SCL_LDR_UNICODE_STRING
#undef SCL_LDR_DATA_TABLE_ENTRY
#undef SCL_PEB_LDR_DATA
#undef SCL_PEB

#ifdef _WIN64
typedef SCL_PEB64 SCL_PEB;
#else
typedef SCL_PEB32 SCL_PEB;
#endif

// Compile-time layout checks for the fields we patch. Keep the struct family
// honest: an accidental field/order mistake fails the build instead of silently
// breaking the debugger detection fixes.
static_assert(offsetof(SCL_PEB32, BeingDebugged) == 0x02, "SCL_PEB32.BeingDebugged offset");
static_assert(offsetof(SCL_PEB32, Ldr) == 0x0C, "SCL_PEB32.Ldr offset");
static_assert(offsetof(SCL_PEB32, ProcessParameters) == 0x10, "SCL_PEB32.ProcessParameters offset");
static_assert(offsetof(SCL_PEB32, NtGlobalFlag) == 0x68, "SCL_PEB32.NtGlobalFlag offset");
static_assert(offsetof(SCL_PEB64, BeingDebugged) == 0x02, "SCL_PEB64.BeingDebugged offset");
static_assert(offsetof(SCL_PEB64, Ldr) == 0x18, "SCL_PEB64.Ldr offset");
static_assert(offsetof(SCL_PEB64, ProcessParameters) == 0x20, "SCL_PEB64.ProcessParameters offset");
static_assert(offsetof(SCL_PEB64, NtGlobalFlag) == 0xBC, "SCL_PEB64.NtGlobalFlag offset");

#ifdef __cplusplus
extern "C" {
#endif

SCL_PEB *scl_get_peb_address(HANDLE hProcess);
PVOID64 scl_get_peb64_address(HANDLE hProcess);

BOOL scl_get_peb(HANDLE hProcess, SCL_PEB *outPeb);
BOOL scl_wow64_get_peb64(HANDLE hProcess, SCL_PEB64 *outPeb64);

BOOL scl_set_peb(HANDLE hProcess, const SCL_PEB *pPeb);
BOOL scl_wow64_set_peb64(HANDLE hProcess, const SCL_PEB64 *pPeb64);

PVOID64 scl_wow64_get_module_handle64(HANDLE hProcess, const wchar_t *moduleName);

DWORD scl_get_heap_flags_offset(BOOL x64);
DWORD scl_get_heap_force_flags_offset(BOOL x64);

#ifdef __cplusplus
}
#endif
