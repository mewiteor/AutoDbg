#pragma once

#include <Windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct scl_settings
{
    BOOL dllNormal;
    BOOL dllStealth;
    BOOL dllUnload;

    BOOL hookGetLocalTime;
    BOOL hookGetSystemTime;
    BOOL hookGetTickCount;
    BOOL hookGetTickCount64;
    BOOL hookKiUserExceptionDispatcher;
    BOOL hookNtClose;
    BOOL hookNtContinue;
    BOOL hookNtCreateThreadEx;
    BOOL hookNtGetContextThread;
    BOOL hookNtQueryInformationProcess;
    BOOL hookNtQueryObject;
    BOOL hookNtQueryPerformanceCounter;
    BOOL hookNtQuerySystemInformation;
    BOOL hookNtQuerySystemTime;
    BOOL hookNtSetContextThread;
    BOOL hookNtSetDebugFilterState;
    BOOL hookNtSetInformationThread;
    BOOL hookNtSetInformationProcess;
    BOOL hookNtUserBlockInput;
    BOOL hookNtUserBuildHwndList;
    BOOL hookNtUserFindWindowEx;
    BOOL hookNtUserQueryWindow;
    BOOL hookNtUserGetForegroundWindow;
    BOOL hookNtYieldExecution;
    BOOL hookOutputDebugStringA;

    BOOL fixPebBeingDebugged;
    BOOL fixPebHeapFlags;
    BOOL fixPebNtGlobalFlag;
    BOOL fixPebStartupInfo;
    BOOL fixPebOsBuildNumber;

    BOOL preventThreadCreation;
    BOOL removeDebugPrivileges;
    BOOL killAntiAttach;
    BOOL malwareRunpeUnpacker;
} scl_settings;

// Fills the settings with the built-in default profile.
void scl_settings_set_defaults(scl_settings *settings);

BOOL scl_settings_hook_dll_needed(const scl_settings *settings);

#ifdef __cplusplus
}
#endif
