#pragma once
#include <windows.h>
#include <ntdll/ntdll.h>

// Used for PEB OsBuildNumber patch and NTDLL version resource patches.
#define FAKE_VERSION 1337

#ifdef __cplusplus
extern "C" {
#endif

void ApplyNtdllVersionPatch(HANDLE hProcess, PVOID Ntdll);

#ifdef __cplusplus
}
#endif
