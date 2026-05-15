#include "framework.h"
#include "Application.h"
#include "TrayManager.h"

#include <shellapi.h>

NOTIFYICONDATAW g_trayData = {};

UINT GetTaskbarCreatedMessage()
{
    static UINT taskbarMessage = RegisterWindowMessageW(L"TaskbarCreated");
    return taskbarMessage;
}

bool AddTrayIcon(HWND hWnd)
{
    ZeroMemory(&g_trayData, sizeof(g_trayData));

    g_trayData.cbSize = sizeof(g_trayData);
    g_trayData.hWnd = hWnd;
    g_trayData.uID = 1;
    g_trayData.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_trayData.uCallbackMessage = WM_TRAYICON;
    g_trayData.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON_SMALL));

    lstrcpyW(g_trayData.szTip, L"Application");

    return Shell_NotifyIconW(NIM_ADD, &g_trayData) == TRUE;
}

void RemoveTrayIcon(HWND hWnd)
{
    if (g_trayData.hWnd == hWnd)
    {
        Shell_NotifyIconW(NIM_DELETE, &g_trayData);
    }
}

void ShowMainWindowFromTray(HWND hWnd)
{
    ShowWindow(hWnd, SW_SHOW);
    ShowWindow(hWnd, SW_RESTORE);
    SetForegroundWindow(hWnd);
}

void HideMainWindowToTray(HWND hWnd)
{
    ShowWindow(hWnd, SW_HIDE);
}

void ShowTrayMenu(HWND hWnd)
{
    HMENU hMenu = CreatePopupMenu();

    if (hMenu == nullptr)
    {
        return;
    }

    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_OPEN, L"Open");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_EXIT, L"Exit");

    POINT pt;
    GetCursorPos(&pt);

    SetForegroundWindow(hWnd);

    TrackPopupMenu(
        hMenu,
        TPM_LEFTALIGN | TPM_BOTTOMALIGN,
        pt.x,
        pt.y,
        0,
        hWnd,
        nullptr);

    DestroyMenu(hMenu);
    PostMessageW(hWnd, WM_NULL, 0, 0);
}

bool ShouldStartHidden(LPCWSTR commandLine)
{
    if (commandLine == nullptr)
    {
        return false;
    }

    return wcsstr(commandLine, L"-tray") != nullptr
        || wcsstr(commandLine, L"/tray") != nullptr
        || wcsstr(commandLine, L"--hidden") != nullptr
        || wcsstr(commandLine, L"--service-child") != nullptr;
}
