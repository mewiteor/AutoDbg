#pragma once

#include "Peb.h"

#define PEB_PATCH_BeingDebugged         0x00000001
#define PEB_PATCH_NtGlobalFlag          0x00000002
#define PEB_PATCH_HeapFlags             0x00000004
#define PEB_PATCH_ProcessParameters     0x00000008
#define PEB_PATCH_OsBuildNumber         0x00000010

#ifdef __cplusplus
extern "C" {
#endif

BOOL scl_peb_patch_process_parameters(SCL_PEB *peb, HANDLE hProcess);
BOOL scl_wow64_peb64_patch_process_parameters(SCL_PEB64 *peb64, HANDLE hProcess);

BOOL scl_peb_patch_heap_flags(SCL_PEB *peb, HANDLE hProcess);
BOOL scl_wow64_peb64_patch_heap_flags(SCL_PEB64 *peb, HANDLE hProcess);

#ifdef __cplusplus
}
#endif
