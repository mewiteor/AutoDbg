#pragma once

#include <Windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCL_USER32_LOADER_MAX_SYSCALLS 32

typedef struct scl_user32_loader_entry
{
    const char *name;
    ULONG_PTR va;
} scl_user32_loader_entry;

typedef struct scl_user32_loader
{
    USHORT OsBuildNumber;
    BOOL NativeX86;
    PUCHAR Win32kUserDll; // win32u.dll if OsBuildNumber >= 14393, user32.dll otherwise
    scl_user32_loader_entry entries[SCL_USER32_LOADER_MAX_SYSCALLS];
    size_t entry_count;
} scl_user32_loader;

BOOL scl_user32_loader_init(scl_user32_loader *loader);
void scl_user32_loader_free(scl_user32_loader *loader);

BOOL scl_user32_loader_find_syscalls(scl_user32_loader *loader, const char *const *syscallNames, size_t count);
ULONG_PTR scl_user32_loader_get_va(const scl_user32_loader *loader, const char *functionName);

#ifdef __cplusplus
}
#endif
