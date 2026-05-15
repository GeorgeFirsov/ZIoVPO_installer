#pragma once

#include <windows.h>

bool AddTrayIcon(HWND hWnd);
void RemoveTrayIcon(HWND hWnd);
void ShowMainWindowFromTray(HWND hWnd);
void HideMainWindowToTray(HWND hWnd);
void ShowTrayMenu(HWND hWnd);
bool ShouldStartHidden(LPCWSTR commandLine);
UINT GetTaskbarCreatedMessage();
