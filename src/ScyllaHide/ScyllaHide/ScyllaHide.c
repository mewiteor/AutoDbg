#include "ScyllaHide.h"
#include <Scylla/Logger.h>
#include <Scylla/OsInfo.h>
#include <Scylla/Settings.h>
#include <Scylla/Version.h>

#include "..\PluginGeneric\Injector.h"
#include <Shlwapi.h>

#include <stdbool.h>
#include <stdio.h>

scl_settings g_settings;

HOOK_DLL_DATA g_hdd = { 0 };

HMODULE hNtdllModule = 0;
bool specialPebFix = false;
DWORD ProcessId = 0;
bool bHooked = false;

static void LogCallback(const char *msg)
{
    printf("[%s] %s\n", SCYLLA_HIDE_NAME_A, msg);
}

static void InitScyllaHidePaths(void)
{
    hNtdllModule = GetModuleHandleW(L"ntdll.dll");

    scl_log_set_cb(SCL_LOG_INFO, LogCallback);
    scl_log_set_cb(SCL_LOG_ERROR, LogCallback);
}

void scylla_hide_setup()
{
    scl_settings_set_defaults(&g_settings);
    InitScyllaHidePaths();

    printf("%s Plugin v%s Copyright (C) 2014 Aguila / cypher\n", SCYLLA_HIDE_NAME_A, SCYLLA_HIDE_VERSION_STRING_A);
}

scl_settings *scylla_hide_get_settings(void)
{
    return &g_settings;
}

void scylla_hide_on_stop()
{
    ZeroMemory(&g_hdd, sizeof(HOOK_DLL_DATA));
    bHooked = false;
    ProcessId = 0;
}

void scylla_hide_on_debug()
{
    if (g_settings.fixPebHeapFlags && specialPebFix)
    {
        StartFixBeingDebugged(ProcessId, false);
        specialPebFix = false;
    }
}

void scylla_hide_on_create_process(DWORD pid, void* start_addr)
{
    scl_log_info(L"pid: %lu, start addr: %p\n", pid, start_addr);
    ProcessId = pid;
    bHooked = false;
    ZeroMemory(&g_hdd, sizeof(HOOK_DLL_DATA));

    if (!start_addr)
    {
        //ATTACH
        if (g_settings.killAntiAttach)
        {
            if (!ApplyAntiAntiAttach(ProcessId))
            {
                MessageBoxW(NULL, L"Anti-Anti-Attach failed", L"Error", MB_ICONERROR);
            }
        }
        //In newest x64dbg version the auto break on attach was removed so ScyllaHide would never inject.
        if (!bHooked)
        {
            ReadNtApiInformation(&g_hdd);

            bHooked = true;
            startInjection(ProcessId, &g_hdd, true);
        }
    }
}

void scylla_hide_on_create_dll(void *dllbase)
{
    if (g_settings.fixPebHeapFlags && dllbase == hNtdllModule)
    {
        StartFixBeingDebugged(ProcessId, true);
        specialPebFix = true;
    }
    if (bHooked)
        startInjection(ProcessId, &g_hdd, false);
}

void scylla_hide_on_breakpoint()
{
    if (!bHooked)
    {
        ReadNtApiInformation(&g_hdd);

        bHooked = true;
        startInjection(ProcessId, &g_hdd, true);
    }
}
