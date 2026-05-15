#include "framework.h"
#include "SecureDesktopConfirm.h"

#include <string>

namespace
{
    struct ConfirmThreadContext
    {
        bool confirmed = false;
    };

    std::wstring BuildDesktopName()
    {
        return L"ZIoVPO_StopConfirm_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
    }

    HDESK OpenDesktopForRestore()
    {
        HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_SWITCHDESKTOP);
        if (desktop != nullptr)
        {
            return desktop;
        }

        return OpenDesktopW(L"Default", 0, FALSE, DESKTOP_SWITCHDESKTOP);
    }

    DWORD WINAPI ConfirmThreadProc(LPVOID parameter)
    {
        ConfirmThreadContext* context = static_cast<ConfirmThreadContext*>(parameter);
        if (context == nullptr)
        {
            return 0;
        }

        HDESK originalDesktop = OpenDesktopForRestore();
        const std::wstring secureDesktopName = BuildDesktopName();

        HDESK secureDesktop = CreateDesktopW(
            secureDesktopName.c_str(),
            nullptr,
            nullptr,
            0,
            DESKTOP_CREATEWINDOW | DESKTOP_SWITCHDESKTOP | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS,
            nullptr);

        if (secureDesktop == nullptr)
        {
            if (originalDesktop != nullptr)
            {
                CloseDesktop(originalDesktop);
            }
            return 0;
        }

        if (!SetThreadDesktop(secureDesktop))
        {
            CloseDesktop(secureDesktop);
            if (originalDesktop != nullptr)
            {
                CloseDesktop(originalDesktop);
            }
            return 0;
        }

        if (SwitchDesktop(secureDesktop))
        {
            const int result = MessageBoxW(
                nullptr,
                L"Are you sure you want to stop protection and exit the application?",
                L"ZIoVPO protection",
                MB_YESNO | MB_ICONWARNING | MB_SETFOREGROUND | MB_SYSTEMMODAL | MB_TOPMOST);

            context->confirmed = (result == IDYES);
        }

        if (originalDesktop != nullptr)
        {
            SwitchDesktop(originalDesktop);
            CloseDesktop(originalDesktop);
        }

        CloseDesktop(secureDesktop);
        return 0;
    }
}

bool ConfirmServiceStopOnSecureDesktop()
{
    ConfirmThreadContext context;

    HANDLE threadHandle = CreateThread(nullptr, 0, ConfirmThreadProc, &context, 0, nullptr);
    if (threadHandle == nullptr)
    {
        return false;
    }

    WaitForSingleObject(threadHandle, INFINITE);
    CloseHandle(threadHandle);

    return context.confirmed;
}
