#include "Injector.h"
#include <Psapi.h>
#include "Scylla/Logger.h"
#include <Scylla/User32Loader.h>
#include <Scylla/OsInfo.h>
#include <Scylla/PebHider.h>
#include <Scylla/Settings.h>
#include <Scylla/Util.h>
#include <HookLibraryData.h>

#include "..\InjectorCLI\\ApplyHooking.h"

extern scl_settings g_settings;

static LPVOID remoteImageBase = 0;

typedef void(__cdecl * t_SetDebuggerBreakpoint)(DWORD_PTR address);
t_SetDebuggerBreakpoint _SetDebuggerBreakpoint = 0;

//anti-attach vars
DWORD ExitThread_addr;
BYTE* DbgUiIssueRemoteBreakin_addr;
DWORD jmpback;
DWORD DbgUiRemoteBreakin_addr;
BYTE* RemoteBreakinPatch;
BYTE OllyRemoteBreakInReplacement[8];
HANDLE hDebuggee;

void ReadNtApiInformation(HOOK_DLL_DATA *hdd)
{
    static const char *const syscallNames[] =
    {
        "NtUserBlockInput",
        "NtUserBuildHwndList",
        "NtUserFindWindowEx",
        "NtUserQueryWindow",
        "NtUserGetForegroundWindow",
        "NtUserGetClassName",
        "NtUserInternalGetWindowText",
        "NtUserGetThreadState"
    };
    scl_user32_loader user32Loader;

    scl_user32_loader_init(&user32Loader);
    if (!scl_user32_loader_find_syscalls(&user32Loader, syscallNames, ARRAYSIZE(syscallNames)))
    {
        scl_log_error(L"Failed to find user32.dll/win32u.dll syscalls!");
        scl_user32_loader_free(&user32Loader);
        return;
    }

    hdd->NtUserBlockInputVA = scl_user32_loader_get_va(&user32Loader, "NtUserBlockInput");
    hdd->NtUserQueryWindowVA = scl_user32_loader_get_va(&user32Loader, "NtUserQueryWindow");
    hdd->NtUserGetForegroundWindowVA = scl_user32_loader_get_va(&user32Loader, "NtUserGetForegroundWindow");
    hdd->NtUserBuildHwndListVA = scl_user32_loader_get_va(&user32Loader, "NtUserBuildHwndList");
    hdd->NtUserFindWindowExVA = scl_user32_loader_get_va(&user32Loader, "NtUserFindWindowEx");
    hdd->NtUserGetClassNameVA = scl_user32_loader_get_va(&user32Loader, "NtUserGetClassName");
    hdd->NtUserInternalGetWindowTextVA = scl_user32_loader_get_va(&user32Loader, "NtUserInternalGetWindowText");
    hdd->NtUserGetThreadStateVA = scl_user32_loader_get_va(&user32Loader, "NtUserGetThreadState");

    scl_log_info(L"Loaded VA for NtUserBlockInput = 0x%p", hdd->NtUserBlockInputVA);
    scl_log_info(L"Loaded VA for NtUserQueryWindow = 0x%p", hdd->NtUserQueryWindowVA);
    scl_log_info(L"Loaded VA for NtUserGetForegroundWindow = 0x%p", hdd->NtUserGetForegroundWindowVA);
    scl_log_info(L"Loaded VA for NtUserBuildHwndList = 0x%p", hdd->NtUserBuildHwndListVA);
    scl_log_info(L"Loaded VA for NtUserFindWindowEx = 0x%p", hdd->NtUserFindWindowExVA);
    scl_log_info(L"Loaded VA for NtUserGetClassName = 0x%p", hdd->NtUserGetClassNameVA);
    scl_log_info(L"Loaded VA for NtUserInternalGetWindowText = 0x%p", hdd->NtUserInternalGetWindowTextVA);
    scl_log_info(L"Loaded VA for NtUserGetThreadState = 0x%p", hdd->NtUserGetThreadStateVA);

    scl_user32_loader_free(&user32Loader);
}

#ifndef _WIN64
void __declspec(naked) handleAntiAttach()
{
    _asm {
        push ebp //stolen bytes
        mov ebp, esp //stolen bytes
        pushad
        mov eax, dword ptr[ebp + 0x8]
        mov hDebuggee, eax
    }

    //write our RemoteBreakIn patch to target memory
    RemoteBreakinPatch = (BYTE*)VirtualAllocEx(hDebuggee, 0, sizeof(OllyRemoteBreakInReplacement), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    WriteProcessMemory(hDebuggee, (LPVOID)RemoteBreakinPatch, OllyRemoteBreakInReplacement, sizeof(OllyRemoteBreakInReplacement), NULL);

    //find push ntdll.DbgUiRemoteBreakin and patch our patch function addr there
    while (*(DWORD*)DbgUiIssueRemoteBreakin_addr != DbgUiRemoteBreakin_addr) {
        DbgUiIssueRemoteBreakin_addr++;
    }
    WriteProcessMemory(GetCurrentProcess(), DbgUiIssueRemoteBreakin_addr, &RemoteBreakinPatch, 4, NULL);
    ULONG oldProtect;
    VirtualProtectEx(hDebuggee, RemoteBreakinPatch, sizeof(OllyRemoteBreakInReplacement), PAGE_EXECUTE_READ, &oldProtect);

    _asm {
        popad
        mov eax, jmpback
        jmp eax
    }
}
#endif

void InstallAntiAttachHook()
{
#ifndef _WIN64
    HANDLE hOlly = GetCurrentProcess();

    DbgUiIssueRemoteBreakin_addr = (BYTE*)GetProcAddress(GetModuleHandleA("ntdll.dll"), "DbgUiIssueRemoteBreakin");
    DbgUiRemoteBreakin_addr = (DWORD)GetProcAddress(GetModuleHandleA("ntdll.dll"), "DbgUiRemoteBreakin");
    ExitThread_addr = (DWORD)GetProcAddress(GetModuleHandleA("kernel32.dll"), "ExitThread");
    jmpback = (DWORD)DbgUiIssueRemoteBreakin_addr;
    jmpback += 5;

    BYTE jmp[1] = { 0xE9 };
    WriteProcessMemory(hOlly, DbgUiIssueRemoteBreakin_addr, &jmp, sizeof(jmp), NULL);
    DWORD patch = (DWORD)handleAntiAttach;
    patch -= (DWORD)DbgUiIssueRemoteBreakin_addr;
    patch -= 5;
    WriteProcessMemory(hOlly, DbgUiIssueRemoteBreakin_addr + 1, &patch, 4, NULL);

    //init our remote breakin patch
    BYTE* p = &OllyRemoteBreakInReplacement[0];
    *p = 0xCC;  //int3
    p++;
    *p = 0x68;  //push
    p++;
    *(DWORD*)(p) = ExitThread_addr;
    p += 4;
    *p = 0xC3; //retn
#endif
}

bool StartFixBeingDebugged(DWORD targetPid, bool setToNull)
{
    HANDLE hProcess = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION, 0, targetPid);
    if (!hProcess)
        return false;

    BOOL result = FALSE;
    do
    {
        SCL_PEB peb;
        if (!scl_get_peb(hProcess, &peb))
            break;

        peb.BeingDebugged = setToNull ? FALSE : TRUE;
        if (!scl_set_peb(hProcess, &peb))
            break;

        if (scl_is_wow64_process(hProcess))
        {
            SCL_PEB64 peb64;
            if (!scl_wow64_get_peb64(hProcess, &peb64))
                break;

            peb.BeingDebugged = setToNull ? FALSE : TRUE;
            if (!scl_wow64_set_peb64(hProcess, &peb64))
                break;
        }

        result = TRUE;
    } while (0);

    CloseHandle(hProcess);
    return result != FALSE;
}

static bool GetProcessInfo(HANDLE hProcess, PPROCESS_SUSPEND_INFO processInfo)
{
    PROCESS_BASIC_INFORMATION basicInfo = { 0 };
    NTSTATUS status = NtQueryInformationProcess(hProcess, ProcessBasicInformation, &basicInfo, sizeof(basicInfo), NULL);
    if (!NT_SUCCESS(status))
        return false;
    ULONG size;
    status = NtQuerySystemInformation(SystemProcessInformation, NULL, 0, &size);
    if (status != STATUS_INFO_LENGTH_MISMATCH)
        return false;
    const PSYSTEM_PROCESS_INFORMATION systemProcessInfo = (PSYSTEM_PROCESS_INFORMATION)RtlAllocateHeap(RtlProcessHeap(), HEAP_ZERO_MEMORY, 2 * size);
    if (systemProcessInfo == NULL)
        return false;
    status = NtQuerySystemInformation(SystemProcessInformation, systemProcessInfo, 2 * size, NULL);
    if (!NT_SUCCESS(status))
    {
        RtlFreeHeap(RtlProcessHeap(), 0, systemProcessInfo);
        return false;
    }

    // Count threads
    ULONG numThreads = 0;
    PSYSTEM_PROCESS_INFORMATION entry = systemProcessInfo;

    while (true)
    {
        if (entry->UniqueProcessId == basicInfo.UniqueProcessId)
        {
            numThreads = entry->NumberOfThreads;
            break;
        }
        if (entry->NextEntryOffset == 0)
            break;
        entry = (PSYSTEM_PROCESS_INFORMATION)((ULONG_PTR)entry + entry->NextEntryOffset);
    }

    if (numThreads == 0)
    {
        RtlFreeHeap(RtlProcessHeap(), 0, systemProcessInfo);
        return false;
    }

    // Fill process info
    processInfo->ProcessId = basicInfo.UniqueProcessId;
    processInfo->ProcessHandle = hProcess;
    processInfo->NumThreads = numThreads;

    // Fill thread IDs
    processInfo->ThreadSuspendInfo = (PTHREAD_SUSPEND_INFO)RtlAllocateHeap(RtlProcessHeap(), HEAP_ZERO_MEMORY, numThreads * sizeof(THREAD_SUSPEND_INFO));
    for (ULONG i = 0; i < numThreads; ++i)
    {
        processInfo->ThreadSuspendInfo[i].ThreadId = entry->Threads[i].ClientId.UniqueThread;
    }

    RtlFreeHeap(RtlProcessHeap(), 0, systemProcessInfo);
    return true;
}

// NtSuspendProcess does not return STATUS_SUSPEND_COUNT_EXCEEDED (or any other error) when one or more thread(s) in the process is/are at the suspend limit.
// This replacement suspends all threads in a process, storing the individual thread suspend statuses. True is returned iff all threads are suspended.
bool SafeSuspendProcess(HANDLE hProcess, PPROCESS_SUSPEND_INFO suspendInfo)
{
    // Get process info
    if (!GetProcessInfo(hProcess, suspendInfo))
        return false;

    for (ULONG i = 0; i < suspendInfo->NumThreads; ++i)
    {
        PTHREAD_SUSPEND_INFO threadSuspendInfo = &suspendInfo->ThreadSuspendInfo[i];
        OBJECT_ATTRIBUTES objectAttributes = RTL_CONSTANT_OBJECT_ATTRIBUTES((PUNICODE_STRING)NULL, 0);
        CLIENT_ID clientId = { suspendInfo->ProcessId, suspendInfo->ThreadSuspendInfo[i].ThreadId };

        // Open the thread by thread ID
        NTSTATUS status = NtOpenThread(&threadSuspendInfo->ThreadHandle, THREAD_SUSPEND_RESUME, &objectAttributes, &clientId);
        if (!NT_SUCCESS(status))
        {
            RtlFreeHeap(RtlProcessHeap(), 0, suspendInfo->ThreadSuspendInfo);
            return false;
        }

        // Suspend the thread, ignoring (but saving) STATUS_SUSPEND_COUNT_EXCEEDED errors
        threadSuspendInfo->SuspendStatus = NtSuspendThread(threadSuspendInfo->ThreadHandle, NULL);
        if (!NT_SUCCESS(threadSuspendInfo->SuspendStatus) && threadSuspendInfo->SuspendStatus != STATUS_SUSPEND_COUNT_EXCEEDED)
        {
            RtlFreeHeap(RtlProcessHeap(), 0, suspendInfo->ThreadSuspendInfo);
            return false;
        }
    }

    return true;
}

// Replacement for NtResumeProcess, to be used with info obtained from a prior call to SafeSuspendProcess
bool SafeResumeProcess(PPROCESS_SUSPEND_INFO suspendInfo)
{
    bool success = true;

    for (ULONG i = 0; i < suspendInfo->NumThreads; ++i)
    {
        if (NT_SUCCESS(suspendInfo->ThreadSuspendInfo[i].SuspendStatus) &&
            !NT_SUCCESS(NtResumeThread(suspendInfo->ThreadSuspendInfo[i].ThreadHandle, NULL)))
            success = false;
        if (!NT_SUCCESS(NtClose(suspendInfo->ThreadSuspendInfo[i].ThreadHandle)))
            success = false;
    }

    RtlFreeHeap(RtlProcessHeap(), 0, suspendInfo->ThreadSuspendInfo);
    return success;
}

bool StartHooking(HANDLE hProcess, HOOK_DLL_DATA *hdd, BYTE * dllMemory, DWORD_PTR imageBase)
{
    hdd->dwProtectedProcessId = GetCurrentProcessId();
    hdd->EnableProtectProcessId = TRUE;

    DWORD peb_flags = 0;
    if (g_settings.fixPebBeingDebugged)
        peb_flags |= PEB_PATCH_BeingDebugged;
    if (g_settings.fixPebHeapFlags)
        peb_flags |= PEB_PATCH_HeapFlags;
    if (g_settings.fixPebNtGlobalFlag)
        peb_flags |= PEB_PATCH_NtGlobalFlag;
    if (g_settings.fixPebStartupInfo)
        peb_flags |= PEB_PATCH_ProcessParameters;
    if (g_settings.fixPebOsBuildNumber)
        peb_flags |= PEB_PATCH_OsBuildNumber;

    ApplyPEBPatch(hProcess, peb_flags);
    if (g_settings.fixPebOsBuildNumber)
        ApplyNtdllVersionPatchLocal(hProcess);

    if (dllMemory == NULL || imageBase == 0)
        return peb_flags != 0; // Not injecting hook DLL

    return ApplyHook(hdd, hProcess, dllMemory, imageBase);
}

void startInjectionProcess(HANDLE hProcess, HOOK_DLL_DATA *hdd, BYTE * dllMemory, bool newProcess)
{
    PROCESS_SUSPEND_INFO suspendInfo;
    if (!SafeSuspendProcess(hProcess, &suspendInfo))
        return;

    const bool injectDll = scl_settings_hook_dll_needed(&g_settings) || hdd->isNtdllHooked || hdd->isKernel32Hooked || hdd->isUserDllHooked;

    DWORD hookDllDataAddressRva = GetDllFunctionAddressRVA(dllMemory, "HookDllData");

    if (!newProcess)
    {
        //g_log.Log(L"Apply hooks again");
        if (injectDll && StartHooking(hProcess, hdd, dllMemory, (DWORD_PTR)remoteImageBase))
        {
            WriteProcessMemory(hProcess, (LPVOID)((DWORD_PTR)hookDllDataAddressRva + (DWORD_PTR)remoteImageBase), hdd, sizeof(HOOK_DLL_DATA), 0);
        }
        else if (!injectDll)
        {
            StartHooking(hProcess, hdd, NULL, 0);
        }
    }
    else
    {
        if (g_settings.removeDebugPrivileges)
        {
            RemoveDebugPrivileges(hProcess);
        }

        RestoreHooks(hdd, hProcess);

        if (injectDll)
        {
            remoteImageBase = MapModuleToProcess(hProcess, dllMemory, true);
            if (remoteImageBase)
            {
                FillHookDllData(hProcess, hdd);

                if (StartHooking(hProcess, hdd, dllMemory, (DWORD_PTR)remoteImageBase) &&
                    WriteProcessMemory(hProcess, (LPVOID)((DWORD_PTR)hookDllDataAddressRva + (DWORD_PTR)remoteImageBase), hdd, sizeof(HOOK_DLL_DATA), 0))
                {
                    scl_log_info(L"Hook injection successful, image base %p", remoteImageBase);
                }
                else
                {
                    scl_log_error(L"Failed to write hook dll data");
                }
            }
            else
            {
                scl_log_error(L"Failed to map image!");
            }
        }
        else
        {
            if (StartHooking(hProcess, hdd, NULL, 0))
                scl_log_info(L"PEB patch successful, hook injection not needed\n");
        }
    }

    SafeResumeProcess(&suspendInfo);
}

void startInjection(DWORD targetPid, HOOK_DLL_DATA *hdd, bool newProcess)
{
    HANDLE hProcess = OpenProcess( PROCESS_SUSPEND_RESUME | PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION | PROCESS_SET_INFORMATION,
        0, targetPid);
    if (hProcess)
    {
        startInjectionProcess(hProcess, hdd, g_dllMemory, newProcess);
        CloseHandle(hProcess);
    }
    else
    {
        scl_log_error(L"Cannot open process handle %d", targetPid);
    }
}

NTSTATUS CreateAndWaitForThread(HANDLE hProcess, LPTHREAD_START_ROUTINE threadStart, PVOID parameter, PHANDLE threadHandle, BOOLEAN suppressDllMains)
{
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    const t_NtCreateThreadEx fpNtCreateThreadEx = (t_NtCreateThreadEx)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateThreadEx");
    if (fpNtCreateThreadEx == NULL)
    {
        // We are on XP/2003 - use CreateRemoteThread
        *threadHandle = CreateRemoteThread(hProcess, NULL, 0, threadStart, parameter, CREATE_SUSPENDED, NULL);
        if (*threadHandle != NULL)
        {
            NtSetInformationThread(*threadHandle, ThreadHideFromDebugger, 0, 0);
            status = STATUS_SUCCESS;
        }
    }
    else
    {
        // Create sneaky thread
        status = fpNtCreateThreadEx(threadHandle,
                                    THREAD_ALL_ACCESS,
                                    NULL,
                                    hProcess,
                                    (PUSER_THREAD_START_ROUTINE)threadStart,
                                    parameter,
                                    THREAD_CREATE_FLAGS_CREATE_SUSPENDED | THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER | (suppressDllMains ? THREAD_CREATE_FLAGS_SUPPRESS_DLLMAINS : 0),
                                    0,
                                    0,
                                    0,
                                    NULL);
    }

    if (NT_SUCCESS(status))
    {
        // Wait for thread to exit
        SetThreadPriority(*threadHandle, THREAD_PRIORITY_TIME_CRITICAL);
        ResumeThread(*threadHandle);
        WaitForSingleObject(*threadHandle, INFINITE);
    }
    return status;
}

LPVOID NormalDllInjection(HANDLE hProcess, const WCHAR * dllPath)
{
    SIZE_T memorySize = (wcslen(dllPath) + 1) * sizeof(WCHAR);

    LPVOID remoteMemory = VirtualAllocEx(hProcess, NULL, memorySize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    LPVOID hModule = NULL;

    if (!remoteMemory)
    {
        scl_log_error(L"DLL INJECTION: VirtualAllocEx failed!");
        return 0;
    }

    if (WriteProcessMemory(hProcess, remoteMemory, dllPath, memorySize, 0))
    {
        HANDLE hThread;
        NTSTATUS status = CreateAndWaitForThread(hProcess, (LPTHREAD_START_ROUTINE)LoadLibraryW, remoteMemory, &hThread, FALSE);
        if (NT_SUCCESS(status))
        {
            GetExitCodeThread(hThread, (LPDWORD)&hModule);

            if (!hModule)
            {
                scl_log_error(L"DLL INJECTION: Failed load library!");
            }

            CloseHandle(hThread);
        }
        else
        {
            scl_log_error(L"DLL INJECTION: Failed to start thread: 0x%08X!", status);
        }
    }
    else
    {
        scl_log_error(L"DLL INJECTION: Failed WriteProcessMemory!");
    }

    VirtualFreeEx(hProcess, remoteMemory, 0, MEM_RELEASE);

    return hModule;
}

DWORD GetAddressOfEntryPoint(BYTE * dllMemory)
{
    PIMAGE_NT_HEADERS ntHeaders = RtlImageNtHeader(dllMemory);
    return HEADER_FIELD(ntHeaders, AddressOfEntryPoint);
}

LPVOID StealthDllInjection(HANDLE hProcess, const WCHAR * dllPath, BYTE * dllMemory)
{
    LPVOID remoteImageBaseOfInjectedDll = 0;

    if (dllMemory)
    {
        remoteImageBaseOfInjectedDll = MapModuleToProcess(hProcess, dllMemory, false);
        if (remoteImageBaseOfInjectedDll)
        {

            DWORD_PTR entryPoint = (DWORD_PTR)GetAddressOfEntryPoint(dllMemory);

            if (entryPoint)
            {
                DWORD_PTR dllMain = entryPoint + (DWORD_PTR)remoteImageBaseOfInjectedDll;

                scl_log_info(L"DLL INJECTION: Starting thread at RVA %p VA %p!", entryPoint, dllMain);

                HANDLE hThread;
                NTSTATUS status = CreateAndWaitForThread(hProcess, (LPTHREAD_START_ROUTINE)dllMain, remoteImageBaseOfInjectedDll, &hThread, TRUE);
                if (NT_SUCCESS(status))
                {
                    CloseHandle(hThread);
                }
                else
                {
                    scl_log_error(L"DLL INJECTION: Failed to start thread: 0x%08X!", status);
                }
            }
        }
        else
        {
            scl_log_error(L"DLL INJECTION: Failed to map image of %s!", dllPath);
        }
    }

    return remoteImageBaseOfInjectedDll;
}

void injectDll(DWORD targetPid, const WCHAR * dllPath)
{
    HANDLE hProcess = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION, 0, targetPid);
    if (hProcess == NULL)
    {
        scl_log_error(L"DLL INJECTION: Cannot open process handle %d", targetPid);
        return;
    }

    BYTE* dllMemory = ReadFileToMemory(dllPath);
    if (dllMemory == NULL)
    {
        scl_log_error(L"DLL INJECTION: Failed to read file %s!", dllPath);
        CloseHandle(hProcess);
        return;
    }

    PIMAGE_NT_HEADERS ntHeaders = RtlImageNtHeader(dllMemory);
    if (ntHeaders == NULL)
    {
        scl_log_error(L"DLL INJECTION: Invalid PE file %s!", dllPath);
        free(dllMemory);
        CloseHandle(hProcess);
        return;
    }

    bool processIsWow64 = scl_is_wow64_process(hProcess);
    if ((scl_is_windows_64() &&
        ((processIsWow64 && ntHeaders->FileHeader.Machine != IMAGE_FILE_MACHINE_I386) ||
        (!processIsWow64 && ntHeaders->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64)))
        ||
        (!scl_is_windows_64() && ntHeaders->FileHeader.Machine != IMAGE_FILE_MACHINE_I386))
    {
        scl_log_error(L"DLL INJECTION: DLL %s is of wrong bitness for process!", dllPath);
        free(dllMemory);
        CloseHandle(hProcess);
        return;
    }

    LPVOID remoteImage = NULL;
    DWORD entryPoint = GetAddressOfEntryPoint(dllMemory);
    if (entryPoint != 0)
        scl_log_info(L"DLL entry point (DllMain) RVA %X!", entryPoint);

    if (g_settings.dllStealth)
    {
        scl_log_info(L"Starting Stealth DLL Injection!");
        remoteImage = StealthDllInjection(hProcess, dllPath, dllMemory);
    }
    else if (g_settings.dllNormal)
    {
        scl_log_info(L"Starting Normal DLL Injection!");
        remoteImage = NormalDllInjection(hProcess, dllPath);
    }
    else
    {
        scl_log_error(L"DLL INJECTION: No injection type selected!");
    }

    if (remoteImage != NULL)
    {
        scl_log_info(L"DLL INJECTION: Injection of %s successful, Imagebase %p", dllPath, remoteImage);

        if (g_settings.dllUnload)
        {
            scl_log_info(L"DLL INJECTION: Unloading Imagebase %p", remoteImage);

            if (g_settings.dllNormal)
            {
                HANDLE hThread;
                NTSTATUS status = CreateAndWaitForThread(hProcess, (LPTHREAD_START_ROUTINE)FreeLibrary, remoteImage, &hThread, FALSE);
                if (NT_SUCCESS(status))
                {
                    CloseHandle(hThread);
                    scl_log_info(L"DLL INJECTION: Unloading Imagebase %p successful", remoteImage);
                }
                else
                {
                    scl_log_error(L"DLL INJECTION: Unloading Imagebase %p FAILED: 0x%08X", remoteImage, status);
                }
            }
            else if (g_settings.dllStealth)
            {
                VirtualFreeEx(hProcess, remoteImage, 0, MEM_RELEASE);
                scl_log_info(L"DLL INJECTION: Unloading Imagebase %p successful", remoteImage);
            }
        }
    }

    free(dllMemory);
    CloseHandle(hProcess);
}

BYTE * ReadFileToMemory(const WCHAR * targetFilePath)
{
    HANDLE hFile;
    DWORD dwBytesRead;
    DWORD FileSize;
    BYTE* FilePtr = 0;

    hFile = CreateFileW(targetFilePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, 0);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        FileSize = GetFileSize(hFile, NULL);
        if (FileSize > 0)
        {
            FilePtr = (BYTE*)calloc(FileSize + 1, 1);
            if (FilePtr)
            {
                if (!ReadFile(hFile, (LPVOID)FilePtr, FileSize, &dwBytesRead, NULL))
                {
                    free(FilePtr);
                    FilePtr = 0;
                }

            }
        }
        CloseHandle(hFile);
    }

    return FilePtr;
}

void FillHookDllData(HANDLE hProcess, HOOK_DLL_DATA *hdd)
{
    hdd->EnablePebBeingDebugged = g_settings.fixPebBeingDebugged;
    hdd->EnablePebHeapFlags = g_settings.fixPebHeapFlags;
    hdd->EnablePebNtGlobalFlag = g_settings.fixPebNtGlobalFlag;
    hdd->EnablePebStartupInfo = g_settings.fixPebStartupInfo;
    hdd->EnablePebOsBuildNumber = g_settings.fixPebOsBuildNumber;
    hdd->EnableOutputDebugStringHook = g_settings.hookOutputDebugStringA;
    hdd->EnableNtSetInformationThreadHook = g_settings.hookNtSetInformationThread;
    hdd->EnableNtQueryInformationProcessHook = g_settings.hookNtQueryInformationProcess;
    hdd->EnableNtQuerySystemInformationHook = g_settings.hookNtQuerySystemInformation;
    hdd->EnableNtQueryObjectHook = g_settings.hookNtQueryObject;
    hdd->EnableNtYieldExecutionHook = g_settings.hookNtYieldExecution;
    hdd->EnableNtCloseHook = g_settings.hookNtClose;
    hdd->EnableNtCreateThreadExHook = g_settings.hookNtCreateThreadEx;
    hdd->EnablePreventThreadCreation = g_settings.preventThreadCreation;
    hdd->EnableNtUserBlockInputHook = g_settings.hookNtUserBlockInput;
    hdd->EnableNtUserFindWindowExHook = g_settings.hookNtUserFindWindowEx;
    hdd->EnableNtUserBuildHwndListHook = g_settings.hookNtUserBuildHwndList;
    hdd->EnableNtUserQueryWindowHook = g_settings.hookNtUserQueryWindow;
    hdd->EnableNtUserGetForegroundWindowHook = g_settings.hookNtUserGetForegroundWindow;
    hdd->EnableNtSetDebugFilterStateHook = g_settings.hookNtSetDebugFilterState;
    hdd->EnableGetTickCountHook = g_settings.hookGetTickCount;
    hdd->EnableGetTickCount64Hook = g_settings.hookGetTickCount64;
    hdd->EnableGetLocalTimeHook = g_settings.hookGetLocalTime;
    hdd->EnableGetSystemTimeHook = g_settings.hookGetSystemTime;
    hdd->EnableNtQuerySystemTimeHook = g_settings.hookNtQuerySystemTime;
    hdd->EnableNtQueryPerformanceCounterHook = g_settings.hookNtQueryPerformanceCounter;
    hdd->EnableNtSetInformationProcessHook = g_settings.hookNtSetInformationProcess;

    hdd->EnableNtGetContextThreadHook = g_settings.hookNtGetContextThread;
    hdd->EnableNtSetContextThreadHook = g_settings.hookNtSetContextThread;
    hdd->EnableNtContinueHook = g_settings.hookNtContinue | g_settings.killAntiAttach;
    hdd->EnableKiUserExceptionDispatcherHook = g_settings.hookKiUserExceptionDispatcher;
    hdd->EnableMalwareRunPeUnpacker = g_settings.malwareRunpeUnpacker;

    hdd->isKernel32Hooked = FALSE;
    hdd->isNtdllHooked = FALSE;
    hdd->isUserDllHooked = FALSE;
}

bool RemoveDebugPrivileges(HANDLE hProcess)
{
    TOKEN_PRIVILEGES Debug_Privileges;

    if (LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &Debug_Privileges.Privileges[0].Luid))
    {
        HANDLE hToken = 0;
        if (OpenProcessToken(hProcess, TOKEN_ADJUST_PRIVILEGES, &hToken))
        {
            Debug_Privileges.Privileges[0].Attributes = 0;
            Debug_Privileges.PrivilegeCount = 1;

            AdjustTokenPrivileges(hToken, FALSE, &Debug_Privileges, 0, NULL, NULL);
            CloseHandle(hToken);
            return true;
        }
    }

    return false;
}

#define DbgBreakPoint_FUNC_SIZE 0x10
#define DbgUiRemoteBreakin_FUNC_SIZE 0x80
#define NtContinue_FUNC_SIZE 0x40

typedef struct _PATCH_FUNC {
    PCHAR funcName;
    PVOID funcAddr;
    SIZE_T funcSize;
} PATCH_FUNC;


PATCH_FUNC patchFunctions[] = {
    {
        "DbgBreakPoint", 0, DbgBreakPoint_FUNC_SIZE
    },
    {
        "DbgUiRemoteBreakin", 0, DbgUiRemoteBreakin_FUNC_SIZE
    },
    {
        "NtContinue", 0, NtContinue_FUNC_SIZE
    }
};

bool ApplyAntiAntiAttach(DWORD targetPid)
{
    bool result = false;
    HANDLE hProcess = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION, 0, targetPid);

    if (!hProcess)
        return result;

    HMODULE hMod = GetModuleHandleW(L"ntdll.dll");

    for (ULONG i = 0; i < _countof(patchFunctions); i++)
    {
        patchFunctions[i].funcAddr = (PVOID)GetProcAddress(hMod, patchFunctions[i].funcName);
    }

    for (ULONG i = 0; i < _countof(patchFunctions); i++)
    {
        ULONG oldProtection;
        if (VirtualProtectEx(hProcess, patchFunctions[i].funcAddr, patchFunctions[i].funcSize, PAGE_EXECUTE_READWRITE, &oldProtection) &&
            WriteProcessMemory(hProcess, patchFunctions[i].funcAddr, patchFunctions[i].funcAddr, patchFunctions[i].funcSize, NULL))
        {
            VirtualProtectEx(hProcess, patchFunctions[i].funcAddr, patchFunctions[i].funcSize, oldProtection, &oldProtection);
            result = true;
        }
        else
        {
            result = false;
            break;
        }
    }

    CloseHandle(hProcess);

    return result;
}
