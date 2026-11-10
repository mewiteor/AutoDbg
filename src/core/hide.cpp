#include "hide.h"
#include "pe.hpp" /* get_peinfo_by_name（原来由 legacy_types.h 声明；类型统一后从这里来） */
#include "types.hpp"
#include "common/dynapi.hpp"   /* get_proc_as：XP+ API 的动态加载（TODO 5.5 / D10） */
#include <winternl.h>

/* 类型统一（删 legacy_types.h）：这些名字现在只有 types.hpp 里的 `autodbg::core::` 那套 */
using autodbg::core::PEInfo;

/* =============================================================================
 * P6.5：本文件归一 —— 原来是「原生路径 / WOW64 路径」两份**逐字重复**的 PEB+Heap
 *   补丁代码（各自 copy 一遍读改写），且 `VirtualProtectEx` 的
 *   「改属性 → 写 → 还原」散在三处手工书写。
 *
 * 现在：
 *   · `PebLayout` 只描述「字段偏移」；`patch_peb(...)` 是唯一一份补丁逻辑；
 *   · `NativeAccess` / `Wow64Access` 只描述「怎么读写目标进程内存」（各 2 个函数）；
 *   · `ScopedProtect` 用 RAII 保证属性一定还原（提前 return / 后续扩展都不会漏）。
 * 行为与旧实现 1:1（含「先按原生布局补一遍，WOW64 目标再按 x64 布局补一遍」的顺序）。
 * ========================================================================== */

#define PEB_BEING_DEBUGGED 2
#define PEB_BUF_SIZE       0xC0

#define X64_PEB_NT_GLOBAL_FLAG 0xBC
#define X64_PEB_PROCESS_HEAP   0x30
#define X64_HEAP_FLAGS         0x70
#define X64_HEAP_FORCE_FLAGS   0x74

#define X86_PEB_NT_GLOBAL_FLAG 0x68
#define X86_PEB_PROCESS_HEAP   0x18
#define X86_HEAP_FLAGS         0x40
#define X86_HEAP_FORCE_FLAGS   0x44

#ifdef _WIN64
#define PEB_M(x) X64_##x
#else
#define PEB_M(x) X86_##x
#endif

/* --- WOW64（32 位宿主 → 64 位目标）用的 ntdll 函数原型 + 降级封装 --- */
typedef NTSTATUS(WINAPI *NtWow64ReadVirtualMemory64FunT)(HANDLE, PVOID64, PVOID, ULONGLONG, PULONGLONG);
typedef NTSTATUS(WINAPI *NtWow64WriteVirtualMemory64FunT)(HANDLE, PVOID64, LPCVOID, ULONGLONG, PULONGLONG);

#ifndef _WIN64
static bool Wow64ReadProcessMemory64(NtWow64ReadVirtualMemory64FunT fun, HANDLE hProcess, PVOID64 address, PVOID buffer, ULONGLONG buffer_size, PULONGLONG bytes_read)
{
    if (fun)
        return NT_SUCCESS(fun(hProcess, address, buffer, buffer_size, bytes_read));
    if ((((DWORD64)address + buffer_size) < (DWORD)(-1)) && (buffer_size <= (DWORD)(-1)))
    {
        SIZE_T bytes_read32 = 0;
        BOOL ret = ReadProcessMemory(hProcess, (PVOID)(ULONG)(ULONG64)address, buffer, (SIZE_T)buffer_size, &bytes_read32);
        if (bytes_read)
            *bytes_read = bytes_read32;
        return ret == TRUE;
    }
    return false;
}

bool Wow64WriteProcessMemory64(NtWow64WriteVirtualMemory64FunT fun, HANDLE hProcess, PVOID64 address, LPCVOID buffer, ULONGLONG buffer_size, PULONGLONG bytes_written)
{
    if (fun)
        return NT_SUCCESS(fun(hProcess, address, buffer, buffer_size, bytes_written));
    else if ((((DWORD64)address + buffer_size) < (DWORD)(-1)) && (buffer_size <= (DWORD)(-1)))
    {
        SIZE_T bytes_written32 = 0;
        BOOL ret = WriteProcessMemory(hProcess, (PVOID)(ULONG)(ULONG64)(address), buffer, (SIZE_T)buffer_size, &bytes_written32);
        if (bytes_written)
            *bytes_written = bytes_written32;
        return ret == TRUE;
    }
    return false;
}
#endif

namespace
{
/* PEB 里四个字段的偏移（x86 / x64 各一套；WOW64 目标用 x64 那套） */
struct PebLayout
{
    DWORD nt_global_flag;   /* PEB->NtGlobalFlag */
    DWORD process_heap;     /* PEB->ProcessHeap */
    DWORD heap_flags;       /* Heap->Flags */
    DWORD heap_force_flags; /* Heap->ForceFlags */
};

#ifdef _WIN64
const PebLayout kLayoutNative = {X64_PEB_NT_GLOBAL_FLAG, X64_PEB_PROCESS_HEAP, X64_HEAP_FLAGS, X64_HEAP_FORCE_FLAGS};
#else
const PebLayout kLayoutNative = {X86_PEB_NT_GLOBAL_FLAG, X86_PEB_PROCESS_HEAP, X86_HEAP_FLAGS, X86_HEAP_FORCE_FLAGS};
const PebLayout kLayoutWow64  = {X64_PEB_NT_GLOBAL_FLAG, X64_PEB_PROCESS_HEAP, X64_HEAP_FLAGS, X64_HEAP_FORCE_FLAGS};
#endif

/* ---- 「读写目标进程内存」的两个实现（接口一致，供 patch_peb 模板复用） ---- */

/* 原生：ReadProcessMemory / WriteProcessMemory（同位数自然可用；x86 目标地址也在 32 位内） */
struct NativeAccess
{
    static bool read(HANDLE hProcess, ULONG_PTR ea, void *buf, size_t size)
    {
        SIZE_T got = 0;
        return ReadProcessMemory(hProcess, (LPCVOID)ea, buf, size, &got) != FALSE;
    }

    static bool write(HANDLE hProcess, ULONG_PTR ea, const void *buf, size_t size)
    {
        SIZE_T put = 0;
        return WriteProcessMemory(hProcess, (LPVOID)ea, buf, size, &put) != FALSE;
    }
};

#ifndef _WIN64
/* WOW64：32 位宿主 → 64 位目标，地址是 64 位，必须走 ntdll 的 NtWow64*VirtualMemory64 */
struct Wow64Access
{
    NtWow64ReadVirtualMemory64FunT  read64;
    NtWow64WriteVirtualMemory64FunT write64;

    bool read(HANDLE hProcess, ULONG_PTR ea, void *buf, size_t size) const
    {
        return Wow64ReadProcessMemory64(read64, hProcess, (PVOID64)ea, buf, (ULONGLONG)size, NULL);
    }

    bool write(HANDLE hProcess, ULONG_PTR ea, const void *buf, size_t size) const
    {
        return Wow64WriteProcessMemory64(write64, hProcess, (PVOID64)ea, buf, (ULONGLONG)size, NULL);
    }
};
#endif

/* 唯一一份「清掉 PEB/Heap 里的调试痕迹」。两步读写的语义与旧代码逐字等价。 */
template <class Access>
void patch_peb(const Access &acc, HANDLE hProcess, ULONG_PTR peb_base, const PebLayout &layout)
{
    BYTE peb[PEB_BUF_SIZE];
    if (!acc.read(hProcess, peb_base, peb, sizeof(peb)))
        return;

    peb[PEB_BEING_DEBUGGED] = 0;
    *(DWORD *)(peb + layout.nt_global_flag) &= ~0x70;

    const ULONG_PTR process_heap = *(const ULONG_PTR *)(peb + layout.process_heap);
    if (process_heap)
    {
        DWORD flags;
        if (acc.read(hProcess, process_heap + layout.heap_flags, &flags, sizeof(flags)))
        {
            flags &= 2;
            acc.write(hProcess, process_heap + layout.heap_flags, &flags, sizeof(flags));
        }
        if (acc.read(hProcess, process_heap + layout.heap_force_flags, &flags, sizeof(flags)))
        {
            flags = 0;
            acc.write(hProcess, process_heap + layout.heap_force_flags, &flags, sizeof(flags));
        }
    }

    acc.write(hProcess, peb_base, peb, sizeof(peb));
}

/* VirtualProtectEx 的 RAII 还原：`改属性 → 写 → 还原` 三步不再手工重复。
 * ❗旧实现还原时把 `old` 同时当入参与出参（能工作但含混），这里语义相同：还原成进函数前的属性。 */
struct ScopedProtect
{
    HANDLE hProcess;
    void  *addr;
    SIZE_T size;
    DWORD  old_protect;
    bool   changed;

    ScopedProtect(HANDLE h, void *a, SIZE_T s, DWORD new_protect)
        : hProcess(h), addr(a), size(s), old_protect(0), changed(false)
    {
        changed = VirtualProtectEx(hProcess, addr, size, new_protect, &old_protect) != FALSE;
    }

    ~ScopedProtect()
    {
        if (changed)
        {
            DWORD dontcare = 0;
            VirtualProtectEx(hProcess, addr, size, old_protect, &dontcare);
        }
    }

    bool ok() const { return changed; }

    ScopedProtect(const ScopedProtect &) = delete;
    ScopedProtect &operator=(const ScopedProtect &) = delete;
};

/* 「在本进程里查目标进程某个 API 的远程地址」：本地 LoadLibrary 拿偏移 + 远程模块基址 */
void *get_remote_proc_addr(HANDLE hProcess, LPCTSTR dll, LPCSTR func)
{
    /* P6.4：`get_peinfo_by_name` 是会话成员；本函数是**自由函数**（hide 入口）→ 显式取当前会话 */
    PEInfo *info = autodbg::core::current_session().get_peinfo_by_name(dll);
    if (!info) return NULL;
    HMODULE hLocal = LoadLibrary(info->Path);
    if (!hLocal) return NULL;
    size_t fun  = (size_t)GetProcAddress(hLocal, func);
    size_t base = (size_t)hLocal;
    FreeLibrary(hLocal);
    return fun ? (void *)(info->base + (fun - base)) : NULL;
}
} /* namespace */

void fix_peb(HANDLE hProcess, BOOL setToNull)
{
    (void)setToNull; /* 保留参数以维持调用点（旧实现也没用它：一律清零 BeingDebugged） */

    PROCESS_BASIC_INFORMATION pbi;
    ULONG                     len;
    HMODULE                   hNtdll = GetModuleHandleA("ntdll.dll");
    if (!hNtdll) return;
    typedef LONG(NTAPI * NtQueryInformationProcessFunT)(HANDLE, DWORD, PVOID, ULONG, PULONG);
    NtQueryInformationProcessFunT NtQueryInformationProcessFun = (NtQueryInformationProcessFunT)GetProcAddress(hNtdll, "NtQueryInformationProcess");
    if (!NtQueryInformationProcessFun) return;
    if (NtQueryInformationProcessFun(hProcess, 0, &pbi, sizeof(pbi), &len) || !pbi.PebBaseAddress) return;

    /* ① 按本进程位数对应的布局补一遍（32 位宿主看 WOW64 目标时，补的是它的 32 位 PEB 视图） */
    NativeAccess native;
    patch_peb(native, hProcess, (ULONG_PTR)pbi.PebBaseAddress, kLayoutNative);

#ifndef _WIN64
    /* ② WOW64 目标：再用 64 位接口补它的 64 位 PEB。
     *
     * ❗ `IsWow64Process` 是 XP+ API：静态导入会让 **Win98 产物加载就失败**
     * （TODO 5.5 / D10）。改走动态加载层：
     *   - 取不到（Win98）→ 当作「不是 Wow64」——在老系统上这正是事实；
     *   - 不用函数内 static 缓存（会引入线程安全初始化的 CRT 辅助符号，零 CRT 下不合适）；
     *     `get_proc` 自带 64 条缓存，重复查询很便宜。 */
    typedef BOOL(WINAPI * IsWow64ProcessT)(HANDLE, PBOOL);
    IsWow64ProcessT pIsWow64Process =
        autodbg::common::get_proc_as<IsWow64ProcessT>("kernel32.dll", "IsWow64Process");
    BOOL isWow64 = FALSE;
    if (pIsWow64Process && pIsWow64Process(hProcess, &isWow64) && isWow64)
    {
        NtQueryInformationProcessFun = (NtQueryInformationProcessFunT)GetProcAddress(hNtdll, "NtWow64QueryInformationProcess64");
        if (!NtQueryInformationProcessFun) return;
        struct
        {
            DWORD64 Reserved1;
            PVOID64 PebBaseAddress;
            DWORD64 Reserved2[2];
            DWORD64 UniqueProcessId;
            DWORD64 Reserved3;
        } pbi64;
        if (NtQueryInformationProcessFun(hProcess, 0, &pbi64, sizeof(pbi64), &len) || !pbi64.PebBaseAddress) return;

        Wow64Access wow64;
        wow64.read64  = (NtWow64ReadVirtualMemory64FunT)GetProcAddress(hNtdll, "NtWow64ReadVirtualMemory64");
        wow64.write64 = (NtWow64WriteVirtualMemory64FunT)GetProcAddress(hNtdll, "NtWow64WriteVirtualMemory64");
        patch_peb(wow64, hProcess, (ULONG_PTR)(uint64_t)pbi64.PebBaseAddress, kLayoutWow64);
    }
#endif
}

void patch_antidebug_api(HANDLE hProcess)
{
#ifdef _WIN64
    const BYTE patchCRDP[] = {0x48, 0x31, 0xC0, 0xC3};
    const BYTE patchGTC[]  = {0x48, 0x31, 0xC0, 0xC3};
#else
    const BYTE patchCRDP[] = {0x33, 0xC0, 0xC2, 0x08, 0x00};
    const BYTE patchGTC[]  = {0x33, 0xC0, 0xC3};
#endif

    struct
    {
        LPCTSTR     dll;
        LPCSTR      func;
        const BYTE *patch;
        SIZE_T      size;
    } apis[] = {
        {TEXT("kernel32.dll"), "CheckRemoteDebuggerPresent", patchCRDP, sizeof(patchCRDP)},
        {TEXT("kernel32.dll"), "GetTickCount",               patchGTC,  sizeof(patchGTC) },
    };

    for (int i = 0; i < (int)(sizeof(apis) / sizeof(apis[0])); i++)
    {
        void *addr = get_remote_proc_addr(hProcess, apis[i].dll, apis[i].func);
        if (!addr) continue;

        ScopedProtect prot(hProcess, addr, apis[i].size, PAGE_EXECUTE_READWRITE);
        if (!prot.ok()) continue;
        WriteProcessMemory(hProcess, addr, apis[i].patch, apis[i].size, NULL);
    }
    FlushInstructionCache(hProcess, NULL, 0);
}
