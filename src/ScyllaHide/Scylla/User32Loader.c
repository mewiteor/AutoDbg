#include "User32Loader.h"
#include "Win32kSyscalls.h"
#include "OsInfo.h"
#include "Logger.h"

#include <string.h>

// Returns the win32k syscall number for a function name
static LONG scl_user32_loader_get_syscall_index(const scl_user32_loader *loader, const char *functionName)
{
    if (loader->OsBuildNumber >= 14393)
    {
        PUCHAR syscallAddress = (PUCHAR)GetProcAddress((HMODULE)loader->Win32kUserDll, functionName);
        PUCHAR address;

        if (syscallAddress == NULL)
            return -1;
        for (address = syscallAddress; address < syscallAddress + 16; ++address)
        {
            if (address[0] == 0xB8 && address[1] != 0xD1)
                return *(PLONG)(address + 1) & 0xFFFF;
        }
        return -1;
    }

    {
        ANSI_STRING searchFunctionName;
        ULONG i;

        RtlInitAnsiString(&searchFunctionName, (PSTR)functionName);
        for (i = 0; i < ARRAYSIZE(Win32kSyscalls); ++i)
        {
            ANSI_STRING tableFunctionName = scl_small_ansi_string_to_ansi_string(&Win32kSyscalls[i].Name);
            if (!RtlEqualString(&tableFunctionName, &searchFunctionName, TRUE))
                continue;
            return scl_win32k_syscall_get_index(&Win32kSyscalls[i], loader->OsBuildNumber, loader->NativeX86);
        }
    }
    return -1;
}

// Scans user32.dll and returns the VA of the function that performs the syscall with the given index
static ULONG_PTR scl_user32_loader_find_syscall_by_index(const scl_user32_loader *loader, LONG win32kSyscallIndex)
{
    const PIMAGE_NT_HEADERS NtHeaders = RtlImageNtHeader(loader->Win32kUserDll);
    const PIMAGE_SECTION_HEADER codeSection = IMAGE_FIRST_SECTION(NtHeaders);
    const PUCHAR start = loader->Win32kUserDll + codeSection->VirtualAddress;
    const PUCHAR end = loader->Win32kUserDll + codeSection->VirtualAddress + codeSection->SizeOfRawData - 16;
    PUCHAR address;

    // Find the syscall pattern for this OS + architecture
    for (address = start; address < end; ++address)
    {
        BOOL isSyscall = FALSE;

        if (address[0] != 0xB8 || (*(PLONG)(address + 1) & 0xFFFF) != win32kSyscallIndex) // mov eax, <syscall num>
            continue;

#ifdef _WIN64
        // For native x64 syscalls 'mov eax, <num>' is always preceded by 'mov r10, rcx'
        if (address[-3] == 0x4C && address[-2] == 0x8B && address[-1] == 0xD1)
        {
            isSyscall = TRUE;
            address -= 3; // Backtrack to first mov
        }
#else
        if (loader->NativeX86)
        {
            // Native x86, old style: mov edx, 7FFE0300h, call [edx]
            if (address[5] == 0xBA && address[6] == 0x00 && address[7] == 0x03 && address[8] == 0xFE &&
                address[9] == 0x7F && address[10] == 0xFF && address[11] == 0x12)
                isSyscall = TRUE;
            // Win 8+ native x86: call leaf_sub, retn X. leaf_sub: mov edx, esp, sysenter, retn
            else if (address[5] == 0xE8 && address[9] == 0x00 && (address[10] == 0xC2 || address[10] == 0xC3))
            {
                ULONG i = 0;
                for (; i < 12; ++i)
                {
                    if (address[11 + i] == 0x8B)
                        break;
                }
                if (address[11 + i] == 0x8B && address[11 + i + 1] == 0xD4 && address[11 + i + 2] == 0x0F &&
                    address[11 + i + 3] == 0x34 && address[11 + i + 4] == 0xC3)
                    isSyscall = TRUE;
            }
        }
        else
        {
            if (
                // Wow64, old style: lea edx, [esp+4] / mov ecx, XXXX, call fs:0C0h
                ((address[5] == 0x8D || address[10] == 0x8D) && (address[5] == 0xB9 || address[9] == 0xB9))
                ||
                // Win 8/8.1 Wow64: call fs:0C0h
                (address[5] == 0x64 && address[6] == 0xFF && address[7] == 0x15 && address[8] == 0xC0 &&
                address[9] == 0x00 && address[10] == 0x00 && address[11] == 0x00)
                ||
                // Win 10 Wow64: mov edx, offset Wow64SystemServiceCall, call edx, retn
                (address[5] == 0xBA && address[10] == 0xFF && address[11] == 0xD2 && (address[12] == 0xC2 || address[12] == 0xC3))
                )
            {
                isSyscall = TRUE;
            }
        }
#endif
        if (isSyscall)
            return (ULONG_PTR)address;
    }
    return 0;
}

BOOL scl_user32_loader_init(scl_user32_loader *loader)
{
    loader->entry_count = 0;
    loader->OsBuildNumber = NtCurrentPeb()->OSBuildNumber;
    loader->NativeX86 = (!scl_is_windows_64() && !scl_is_wow64_process(NtCurrentProcess));
    loader->Win32kUserDll = (PUCHAR)LoadLibraryExW(loader->OsBuildNumber >= 14393 ? L"win32u.dll" : L"user32.dll",
        NULL, DONT_RESOLVE_DLL_REFERENCES | LOAD_IGNORE_CODE_AUTHZ_LEVEL |
        (loader->OsBuildNumber >= 6002 ? LOAD_LIBRARY_SEARCH_SYSTEM32 : 0));
    return TRUE;
}

void scl_user32_loader_free(scl_user32_loader *loader)
{
    if (loader->Win32kUserDll != NULL)
    {
        FreeLibrary((HMODULE)loader->Win32kUserDll);
        loader->Win32kUserDll = NULL;
    }
}

ULONG_PTR scl_user32_loader_get_va(const scl_user32_loader *loader, const char *functionName)
{
    size_t i;

    for (i = 0; i < loader->entry_count; i++)
    {
        if (strcmp(loader->entries[i].name, functionName) == 0)
            return loader->entries[i].va;
    }
    return 0;
}

// Finds the requested user32/win32u syscalls by name for later retrieval with scl_user32_loader_get_va
BOOL scl_user32_loader_find_syscalls(scl_user32_loader *loader, const char *const *syscallNames, size_t count)
{
    size_t i;
    LONG syscallNums[SCL_USER32_LOADER_MAX_SYSCALLS];
    ULONG_PTR BlockInputVa;
    BOOL check;

    if (loader->Win32kUserDll == NULL) // Failed to load user32.dll or win32u.dll
        return FALSE;
    if (loader->OsBuildNumber < 2600) // Unsupported or unknown OS
        return FALSE;
    if (count > SCL_USER32_LOADER_MAX_SYSCALLS)
        return FALSE;

    if (loader->OsBuildNumber >= 14393)
    {
        // On >= 10.0.14393.0 we can simply get the VAs from win32u.dll
        for (i = 0; i < count; i++)
        {
            ULONG_PTR syscallAddress = (ULONG_PTR)GetProcAddress((HMODULE)loader->Win32kUserDll, syscallNames[i]);
            if (syscallAddress == 0)
                return FALSE;
            loader->entries[i].name = syscallNames[i];
            loader->entries[i].va = syscallAddress;
        }
        loader->entry_count = count;
        return TRUE;
    }

    // OS is < 14393. Get the syscall indices of the functions that we want the VAs of
    for (i = 0; i < count; i++)
    {
        LONG syscallNum = scl_user32_loader_get_syscall_index(loader, syscallNames[i]);
        if (syscallNum == -1)
            return FALSE;
        syscallNums[i] = syscallNum;
    }

    // Find the VAs of the functions we want
    for (i = 0; i < count; i++)
    {
        ULONG_PTR syscallAddress = scl_user32_loader_find_syscall_by_index(loader, syscallNums[i]);
        if (syscallAddress != 0)
        {
            loader->entries[i].name = syscallNames[i];
            loader->entries[i].va = syscallAddress;
        }
        else
        {
            scl_log_error(L"Address of syscall %hs with index %u not found", syscallNames[i], (ULONG)syscallNums[i]);
            return FALSE;
        }
    }
    loader->entry_count = count;

    // Sanity check the NtUserBlockInput VA as this is an exported syscall
    BlockInputVa = (ULONG_PTR)GetProcAddress((HMODULE)loader->Win32kUserDll, "BlockInput");
    if (BlockInputVa == 0)
        return FALSE;
    check = (scl_user32_loader_get_va(loader, "NtUserBlockInput") == BlockInputVa);
    if (!check)
        scl_log_error(L"GetUserSyscallVa returned incorrect address 0x%p (expected 0x%p)!", scl_user32_loader_get_va(loader, "NtUserBlockInput"), BlockInputVa);

    return check;
}
