#pragma once

#include <windows.h>

extern SERVICE_STATUS g_ServiceStatus;
extern SERVICE_STATUS_HANDLE g_StatusHandle;
extern HANDLE g_StopEvent;

void SetServiceStatusState(DWORD currentState, DWORD win32ExitCode = NO_ERROR, DWORD waitHint = 0);
DWORD WINAPI ServiceHandler(DWORD control, DWORD eventType, LPVOID eventData, LPVOID context);
void WINAPI ServiceMain(DWORD argc, LPWSTR* argv);