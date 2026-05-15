#include "framework.h"
#include "SingleInstance.h"

#include <Lmcons.h>
#include <string>

#pragma comment(lib, "Advapi32.lib")

HANDLE g_applicationMutex = nullptr;

std::wstring GetMutexName()
{
    wchar_t userName[UNLEN + 1] = L"";
    DWORD userNameSize = UNLEN + 1;

    if (!GetUserNameW(userName, &userNameSize))
    {
        lstrcpyW(userName, L"DefaultUser");
    }

    std::wstring mutexName = L"Local\\Application_";
    mutexName += userName;

    return mutexName;
}

bool CreateApplicationMutex()
{
    std::wstring mutexName = GetMutexName();

    g_applicationMutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());

    if (g_applicationMutex == nullptr)
    {
        return false;
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(g_applicationMutex);
        g_applicationMutex = nullptr;
        return false;
    }

    return true;
}

void CloseApplicationMutex()
{
    if (g_applicationMutex != nullptr)
    {
        CloseHandle(g_applicationMutex);
        g_applicationMutex = nullptr;
    }
}
