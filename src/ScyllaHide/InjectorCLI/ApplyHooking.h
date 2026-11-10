#pragma once
#include <windows.h>
#include "..\HookLibrary\HookMain.h"

#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

void ApplyPEBPatch(HANDLE hProcess, DWORD flags);
void ApplyNtdllVersionPatchLocal(HANDLE hProcess);
bool ApplyHook(HOOK_DLL_DATA * hdd, HANDLE hProcess, BYTE * dllMemory, DWORD_PTR imageBase);
void RestoreHooks(HOOK_DLL_DATA * hdd, HANDLE hProcess);

#ifdef __cplusplus
}
#endif
