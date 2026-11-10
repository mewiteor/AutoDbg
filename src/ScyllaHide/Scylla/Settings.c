#include "Settings.h"

void scl_settings_set_defaults(scl_settings *settings)
{
    settings->dllNormal = TRUE;
    settings->dllStealth = FALSE;
    settings->dllUnload = FALSE;

    settings->hookGetLocalTime = TRUE;
    settings->hookGetSystemTime = TRUE;
    settings->hookGetTickCount = TRUE;
    settings->hookGetTickCount64 = TRUE;
    settings->hookKiUserExceptionDispatcher = TRUE;
    settings->hookNtClose = TRUE;
    settings->hookNtContinue = TRUE;
    settings->hookNtCreateThreadEx = TRUE;
    settings->hookNtGetContextThread = TRUE;
    settings->hookNtQueryInformationProcess = TRUE;
    settings->hookNtQueryObject = TRUE;
    settings->hookNtQueryPerformanceCounter = TRUE;
    settings->hookNtQuerySystemInformation = TRUE;
    settings->hookNtQuerySystemTime = TRUE;
    settings->hookNtSetContextThread = TRUE;
    settings->hookNtSetDebugFilterState = TRUE;
    settings->hookNtSetInformationThread = TRUE;
    settings->hookNtSetInformationProcess = TRUE;
    settings->hookNtUserBlockInput = TRUE;
    settings->hookNtUserBuildHwndList = TRUE;
    settings->hookNtUserFindWindowEx = TRUE;
    settings->hookNtUserQueryWindow = TRUE;
    settings->hookNtUserGetForegroundWindow = TRUE;
    settings->hookNtYieldExecution = TRUE;
    settings->hookOutputDebugStringA = TRUE;

    settings->fixPebBeingDebugged = TRUE;
    settings->fixPebHeapFlags = TRUE;
    settings->fixPebNtGlobalFlag = TRUE;
    settings->fixPebStartupInfo = TRUE;
    settings->fixPebOsBuildNumber = TRUE;

    settings->preventThreadCreation = FALSE;
    settings->removeDebugPrivileges = TRUE;
    settings->killAntiAttach = TRUE;
    settings->malwareRunpeUnpacker = FALSE;
}

BOOL scl_settings_hook_dll_needed(const scl_settings *settings)
{
    return
        settings->hookGetLocalTime ||
        settings->hookGetSystemTime ||
        settings->hookGetTickCount ||
        settings->hookGetTickCount64 ||
        settings->hookKiUserExceptionDispatcher ||
        settings->hookNtClose ||
        settings->hookNtContinue ||
        settings->hookNtCreateThreadEx ||
        settings->hookNtGetContextThread ||
        settings->hookNtQueryInformationProcess ||
        settings->hookNtQueryObject ||
        settings->hookNtQueryPerformanceCounter ||
        settings->hookNtQuerySystemInformation ||
        settings->hookNtQuerySystemTime ||
        settings->hookNtSetContextThread ||
        settings->hookNtSetDebugFilterState ||
        settings->hookNtSetInformationThread ||
        settings->hookNtSetInformationProcess ||
        settings->hookNtUserBlockInput ||
        settings->hookNtUserBuildHwndList ||
        settings->hookNtUserFindWindowEx ||
        settings->hookNtUserQueryWindow ||
        settings->hookNtUserGetForegroundWindow ||
        settings->hookNtYieldExecution ||
        settings->hookOutputDebugStringA ||
        settings->preventThreadCreation ||
        settings->malwareRunpeUnpacker;
}
