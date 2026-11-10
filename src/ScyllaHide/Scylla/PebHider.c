#include "PebHider.h"
#include "NtApiShim.h"
#include "OsInfo.h"
#include "Util.h"

#include <stdlib.h>

#define HEAP_VALIDATE_ALL_ENABLED       0x20000000
#define HEAP_CAPTURE_STACK_BACKTRACES   0x08000000

// Flags set by RtlDebugCreateHeap
#define RTLDEBUGCREATEHEAP_HEAP_FLAGS   (HEAP_TAIL_CHECKING_ENABLED | HEAP_FREE_CHECKING_ENABLED | HEAP_SKIP_VALIDATION_CHECKS)

// Additional debug flags that may be set depending on NtGlobalFlags
#define NTGLOBALFLAGS_HEAP_FLAGS        (HEAP_DISABLE_COALESCE_ON_FREE | HEAP_FREE_CHECKING_ENABLED | HEAP_TAIL_CHECKING_ENABLED | \
                                        HEAP_VALIDATE_ALL_ENABLED | HEAP_VALIDATE_PARAMETERS_ENABLED | HEAP_CAPTURE_STACK_BACKTRACES)

// The set of heap flags to clear is the union of flags set by RtlDebugCreateHeap and NtGlobalFlags
#define HEAP_CLEARABLE_FLAGS            (RTLDEBUGCREATEHEAP_HEAP_FLAGS | NTGLOBALFLAGS_HEAP_FLAGS)

// Only a subset of possible flags passed to RtlCreateHeap persists into force flags
#define HEAP_VALID_FORCE_FLAGS          (HEAP_NO_SERIALIZE | HEAP_GENERATE_EXCEPTIONS | HEAP_ZERO_MEMORY | HEAP_REALLOC_IN_PLACE_ONLY | \
                                        HEAP_VALIDATE_PARAMETERS_ENABLED | HEAP_VALIDATE_ALL_ENABLED | HEAP_TAIL_CHECKING_ENABLED | \
                                        HEAP_CREATE_ALIGN_16 | HEAP_FREE_CHECKING_ENABLED)

// The set of force flags to clear is the intersection of valid force flags and the debug flags
#define HEAP_CLEARABLE_FORCE_FLAGS      (HEAP_CLEARABLE_FLAGS & HEAP_VALID_FORCE_FLAGS)

BOOL scl_peb_patch_process_parameters(SCL_PEB *peb, HANDLE hProcess)
{
    SCL_RTL_USER_PROCESS_PARAMETERS_NATIVE rupp;
    DWORD_PTR patch_size;

    if (ReadProcessMemory(hProcess, (PVOID)peb->ProcessParameters, &rupp, sizeof(rupp), NULL) == FALSE)
        return FALSE;

    // Some debuggers manipulate StartUpInfo to start the debugged process and therefore can be detected...
    patch_size = (DWORD_PTR)&rupp.WindowFlags - (DWORD_PTR)&rupp.StartingX;
    ZeroMemory(&rupp.WindowFlags, patch_size);

    // https://github.com/x64dbg/ScyllaHide/issues/99
    rupp.WindowFlags = STARTF_USESHOWWINDOW;
    rupp.ShowWindowFlags = SW_SHOWNORMAL;

    // If the debugger used IFEO, the app doesn't need to know that
    rupp.Flags |= RTL_USER_PROCESS_PARAMETERS_IMAGE_KEY_MISSING;

    return WriteProcessMemory(hProcess, (PVOID)peb->ProcessParameters, &rupp, sizeof(rupp), NULL) == TRUE;
}

BOOL scl_wow64_peb64_patch_process_parameters(SCL_PEB64 *peb64, HANDLE hProcess)
{
#ifndef _WIN64
    SCL_RTL_USER_PROCESS_PARAMETERS64 rupp;
    DWORD_PTR patch_size;

    if (!scl_wow64_read_process_memory64(hProcess, (PVOID64)peb64->ProcessParameters, (PVOID)&rupp, sizeof(rupp), NULL))
        return FALSE;

    // Some debuggers manipulate StartUpInfo to start the debugged process and therefore can be detected...
    patch_size = (DWORD_PTR)&rupp.WindowFlags - (DWORD_PTR)&rupp.StartingX;
    ZeroMemory(&rupp.WindowFlags, patch_size);

    // https://github.com/x64dbg/ScyllaHide/issues/99
    rupp.WindowFlags = STARTF_USESHOWWINDOW;
    rupp.ShowWindowFlags = SW_SHOWNORMAL;

    // If the debugger used IFEO, the app doesn't need to know that
    rupp.Flags |= RTL_USER_PROCESS_PARAMETERS_IMAGE_KEY_MISSING;

    return scl_wow64_write_process_memory64(hProcess, (PVOID64)peb64->ProcessParameters, &rupp, sizeof(rupp), NULL);
#else
    UNREFERENCED_PARAMETER(peb64);
    UNREFERENCED_PARAMETER(hProcess);
    return FALSE;
#endif
}

BOOL scl_peb_patch_heap_flags(SCL_PEB *peb, HANDLE hProcess)
{
#ifdef _WIN64
    const BOOL is_x64 = TRUE;
#else
    const BOOL is_x64 = FALSE;
#endif
    PVOID *heaps;
    BYTE heap[0x100]; // hacky
    DWORD i;
    BOOL result = TRUE;

    heaps = (PVOID *)malloc(peb->NumberOfHeaps * sizeof(PVOID));
    if (heaps == NULL && peb->NumberOfHeaps != 0)
        return FALSE;

    if (ReadProcessMemory(hProcess, (PVOID)peb->ProcessHeaps, heaps, peb->NumberOfHeaps * sizeof(PVOID), NULL) == FALSE)
    {
        free(heaps);
        return FALSE;
    }

    for (i = 0; i < peb->NumberOfHeaps; i++)
    {
        DWORD *flags;
        DWORD *force_flags;

        if (ReadProcessMemory(hProcess, heaps[i], heap, sizeof(heap), NULL) == FALSE)
        {
            result = FALSE;
            break;
        }

        flags = (DWORD *)(heap + scl_get_heap_flags_offset(is_x64));
        force_flags = (DWORD *)(heap + scl_get_heap_force_flags_offset(is_x64));

        *flags &= ~HEAP_CLEARABLE_FLAGS;
        *force_flags &= ~HEAP_CLEARABLE_FORCE_FLAGS;

        if (WriteProcessMemory(hProcess, heaps[i], heap, sizeof(heap), NULL) == FALSE)
        {
            result = FALSE;
            break;
        }
    }

    free(heaps);
    return result;
}

BOOL scl_wow64_peb64_patch_heap_flags(SCL_PEB64 *peb, HANDLE hProcess)
{
    PVOID64 *heaps;
    BYTE heap[0x100]; // hacky
    DWORD i;
    BOOL result = TRUE;

    heaps = (PVOID64 *)malloc(peb->NumberOfHeaps * sizeof(PVOID64));
    if (heaps == NULL && peb->NumberOfHeaps != 0)
        return FALSE;

    if (scl_wow64_read_process_memory64(hProcess, (PVOID64)peb->ProcessHeaps, (PVOID)heaps, peb->NumberOfHeaps * sizeof(PVOID64), NULL) == FALSE)
    {
        free(heaps);
        return FALSE;
    }

    for (i = 0; i < peb->NumberOfHeaps; i++)
    {
        DWORD *flags;
        DWORD *force_flags;

        if (scl_wow64_read_process_memory64(hProcess, (PVOID64)heaps[i], (PVOID)heap, sizeof(heap), NULL) == FALSE)
        {
            result = FALSE;
            break;
        }

        flags = (DWORD *)(heap + scl_get_heap_flags_offset(TRUE));
        force_flags = (DWORD *)(heap + scl_get_heap_force_flags_offset(TRUE));

        *flags &= ~HEAP_CLEARABLE_FLAGS;
        *force_flags &= ~HEAP_CLEARABLE_FORCE_FLAGS;

        if (scl_wow64_write_process_memory64(hProcess, (PVOID64)heaps[i], (PVOID)heap, sizeof(heap), NULL) == FALSE)
        {
            result = FALSE;
            break;
        }
    }

    free(heaps);
    return result;
}
