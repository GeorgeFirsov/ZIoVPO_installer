#include "framework.h"
#include "Application.h"
#include "Resource.h"
#include "ServiceControlClient.h"
#include "SecureDesktopConfirm.h"
#include "TrayManager.h"
#include "SingleInstance.h"

#include <string>

#define MAX_LOADSTRING 100

ATOM MyRegisterClass(HINSTANCE hInstance);
BOOL InitInstance(HINSTANCE hInstance, int nCmdShow, LPCWSTR lpCmdLine);
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

namespace
{
    constexpr UINT_PTR kStatePollingTimerId = 1;
    constexpr UINT kStatePollingIntervalMs = 5000;

    struct AppUiState
    {
        bool authenticated = false;
        bool hasLicense = false;
        bool blocked = true;
        bool avDatabaseLoaded = false;
        long avRecordCount = 0;
        std::wstring username;
        std::wstring expirationDate;
        std::wstring avReleaseDate;
        std::wstring errorMessage;
    };

    struct AppControls
    {
        HWND statusText = nullptr;
        HWND userText = nullptr;
        HWND licenseText = nullptr;
        HWND antivirusStatusText = nullptr;
        HWND avDatabaseText = nullptr;
        HWND errorText = nullptr;

        HWND loginLabel = nullptr;
        HWND loginEdit = nullptr;
        HWND passwordLabel = nullptr;
        HWND passwordEdit = nullptr;
        HWND loginButton = nullptr;
        HWND logoutButton = nullptr;

        HWND activationLabel = nullptr;
        HWND activationEdit = nullptr;
        HWND activateButton = nullptr;

        HWND scanFileLabel = nullptr;
        HWND scanFileEdit = nullptr;
        HWND scanFileButton = nullptr;
        HWND scanDirLabel = nullptr;
        HWND scanDirEdit = nullptr;
        HWND scanDirButton = nullptr;
        HWND scanFixedButton = nullptr;

        HWND scheduleLabel = nullptr;
        HWND schedulePathEdit = nullptr;
        HWND scheduleIntervalEdit = nullptr;
        HWND scheduleEnableButton = nullptr;
        HWND scheduleDisableButton = nullptr;
        HWND scheduleResultButton = nullptr;

        HWND monitorLabel = nullptr;
        HWND monitorDirEdit = nullptr;
        HWND monitorEnableButton = nullptr;
        HWND monitorDisableButton = nullptr;
        HWND monitorResultButton = nullptr;

        HWND scanResultEdit = nullptr;
    };

    HINSTANCE g_instance = nullptr;
    WCHAR g_windowTitle[MAX_LOADSTRING] = {};
    WCHAR g_windowClass[MAX_LOADSTRING] = {};
    HWND g_mainWindow = nullptr;
    AppControls g_controls;
    AppUiState g_state;

    void SetControlText(HWND control, const std::wstring& text)
    {
        if (control != nullptr)
        {
            SetWindowTextW(control, text.c_str());
        }
    }

    void ShowControl(HWND control, bool visible)
    {
        if (control != nullptr)
        {
            ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
        }
    }

    void EnableControl(HWND control, bool enabled)
    {
        if (control != nullptr)
        {
            EnableWindow(control, enabled ? TRUE : FALSE);
        }
    }

    std::wstring ReadWindowText(HWND control)
    {
        if (control == nullptr)
        {
            return L"";
        }

        int length = GetWindowTextLengthW(control);
        if (length <= 0)
        {
            return L"";
        }

        std::wstring text(static_cast<size_t>(length) + 1, L'\0');
        GetWindowTextW(control, text.data(), length + 1);
        text.resize(static_cast<size_t>(length));
        return text;
    }

    void ClearEdit(HWND control)
    {
        if (control != nullptr)
        {
            SetWindowTextW(control, L"");
        }
    }

    long ReadLongFromEdit(HWND control)
    {
        const std::wstring text = ReadWindowText(control);
        try
        {
            return std::stol(text);
        }
        catch (...)
        {
            return 0;
        }
    }

    void SetScanResult(const std::wstring& text)
    {
        SetControlText(g_controls.scanResultEdit, text);
    }

    std::wstring MapLoginStatusToMessage(DWORD statusCode)
    {
        switch (statusCode)
        {
        case ERROR_SUCCESS: return L"";
        case ERROR_LOGON_FAILURE: return L"Authentication failed. Check username and password.";
        case ERROR_INVALID_DATA: return L"Server returned an unexpected login response.";
        case ERROR_INVALID_PARAMETER: return L"Required login data was not provided.";
        case ERROR_GEN_FAILURE: return L"Login failed. Service or server error.";
        case 12002: return L"Login failed: server timeout.";
        case 12007: return L"Login failed: server address was not found.";
        case 12029: return L"Login failed: no connection to server.";
        case 12037: return L"Login failed: SSL certificate error.";
        case 12175: return L"Login failed: TLS/SSL error.";
        default: return L"Login failed. Error code: " + std::to_wstring(statusCode);
        }
    }

    std::wstring MapActivationStatusToMessage(DWORD statusCode)
    {
        switch (statusCode)
        {
        case ERROR_SUCCESS: return L"";
        case ERROR_NOT_FOUND: return L"Activation failed. Check activation code.";
        case ERROR_ACCESS_DENIED: return L"Activation failed. User is not authenticated.";
        case ERROR_INVALID_PARAMETER: return L"Activation failed. Required parameters were not provided.";
        case ERROR_INVALID_DATA: return L"Server returned an unexpected activation response.";
        case ERROR_GEN_FAILURE: return L"Activation failed. Service or server error.";
        case 12002: return L"Activation failed: server timeout.";
        case 12007: return L"Activation failed: server address was not found.";
        case 12029: return L"Activation failed: no connection to server.";
        case 12037: return L"Activation failed: SSL certificate error.";
        case 12175: return L"Activation failed: TLS/SSL error.";
        default: return L"Activation failed. Error code: " + std::to_wstring(statusCode);
        }
    }

    std::wstring MapScanStatusToMessage(DWORD statusCode)
    {
        switch (statusCode)
        {
        case ERROR_SUCCESS: return L"";
        case ERROR_ACCESS_DENIED: return L"Operation is not available: user is not logged in or license is not active.";
        case ERROR_INVALID_PARAMETER: return L"Required parameters were not provided.";
        case ERROR_INVALID_DATA: return L"Antivirus database is corrupted or has invalid format.";
        case ERROR_GEN_FAILURE: return L"Service or server error.";
        default: return L"Error code: " + std::to_wstring(statusCode);
        }
    }

    void UpdateWindowControls()
    {
        const bool showAuth = !g_state.authenticated;
        const bool showActivation = g_state.authenticated && !g_state.hasLicense;
        const bool antivirusUnlocked = g_state.authenticated && g_state.hasLicense && !g_state.blocked;

        SetControlText(g_controls.statusText,
            showAuth
            ? L"User is not authenticated."
            : (g_state.hasLicense ? L"User is authenticated." : L"User is authenticated. License is required."));

        SetControlText(g_controls.userText,
            g_state.authenticated ? (L"Login: " + g_state.username) : L"Login: -");

        SetControlText(g_controls.licenseText,
            g_state.hasLicense ? (L"License expiration date: " + g_state.expirationDate) : L"License is missing.");

        SetControlText(g_controls.antivirusStatusText,
            antivirusUnlocked ? L"Antivirus functionality is unlocked." : L"Antivirus functionality is locked.");

        SetControlText(g_controls.avDatabaseText,
            antivirusUnlocked
            ? (g_state.avDatabaseLoaded
                ? (L"Antivirus database: release date " + g_state.avReleaseDate + L", records: " + std::to_wstring(g_state.avRecordCount))
                : L"Antivirus database: not loaded")
            : L"Antivirus database: unavailable without active license");

        SetControlText(g_controls.errorText, g_state.errorMessage);

        ShowControl(g_controls.loginLabel, showAuth);
        ShowControl(g_controls.loginEdit, showAuth);
        ShowControl(g_controls.passwordLabel, showAuth);
        ShowControl(g_controls.passwordEdit, showAuth);
        ShowControl(g_controls.loginButton, showAuth);

        ShowControl(g_controls.logoutButton, g_state.authenticated);

        ShowControl(g_controls.activationLabel, showActivation);
        ShowControl(g_controls.activationEdit, showActivation);
        ShowControl(g_controls.activateButton, showActivation);

        EnableControl(g_controls.scanFileEdit, antivirusUnlocked);
        EnableControl(g_controls.scanFileButton, antivirusUnlocked);
        EnableControl(g_controls.scanDirEdit, antivirusUnlocked);
        EnableControl(g_controls.scanDirButton, antivirusUnlocked);
        EnableControl(g_controls.scanFixedButton, antivirusUnlocked);
        EnableControl(g_controls.schedulePathEdit, antivirusUnlocked);
        EnableControl(g_controls.scheduleIntervalEdit, antivirusUnlocked);
        EnableControl(g_controls.scheduleEnableButton, antivirusUnlocked);
        EnableControl(g_controls.scheduleDisableButton, antivirusUnlocked);
        EnableControl(g_controls.scheduleResultButton, antivirusUnlocked);
        EnableControl(g_controls.monitorDirEdit, antivirusUnlocked);
        EnableControl(g_controls.monitorEnableButton, antivirusUnlocked);
        EnableControl(g_controls.monitorDisableButton, antivirusUnlocked);
        EnableControl(g_controls.monitorResultButton, antivirusUnlocked);
    }

    void CreateControls(HWND hWnd)
    {
        g_controls.statusText = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
            20, 20, 640, 20, hWnd, reinterpret_cast<HMENU>(IDC_STATUS_TEXT), g_instance, nullptr);

        g_controls.userText = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
            20, 50, 640, 20, hWnd, reinterpret_cast<HMENU>(IDC_USER_TEXT), g_instance, nullptr);

        g_controls.licenseText = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
            20, 80, 640, 20, hWnd, reinterpret_cast<HMENU>(IDC_LICENSE_TEXT), g_instance, nullptr);

        g_controls.antivirusStatusText = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
            20, 110, 760, 20, hWnd, reinterpret_cast<HMENU>(IDC_ANTIVIRUS_STATUS), g_instance, nullptr);

        g_controls.avDatabaseText = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
            20, 140, 840, 20, hWnd, reinterpret_cast<HMENU>(IDC_AV_DATABASE_TEXT), g_instance, nullptr);

        g_controls.errorText = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
            20, 165, 840, 40, hWnd, reinterpret_cast<HMENU>(IDC_ERROR_TEXT), g_instance, nullptr);

        g_controls.loginLabel = CreateWindowW(L"STATIC", L"Login:", WS_CHILD | WS_VISIBLE,
            20, 220, 100, 20, hWnd, reinterpret_cast<HMENU>(IDC_LOGIN_LABEL), g_instance, nullptr);

        g_controls.loginEdit = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            130, 216, 220, 24, hWnd, reinterpret_cast<HMENU>(IDC_LOGIN_EDIT), g_instance, nullptr);

        g_controls.passwordLabel = CreateWindowW(L"STATIC", L"Password:", WS_CHILD | WS_VISIBLE,
            20, 255, 100, 20, hWnd, reinterpret_cast<HMENU>(IDC_PASSWORD_LABEL), g_instance, nullptr);

        g_controls.passwordEdit = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | ES_PASSWORD,
            130, 251, 220, 24, hWnd, reinterpret_cast<HMENU>(IDC_PASSWORD_EDIT), g_instance, nullptr);

        g_controls.loginButton = CreateWindowW(L"BUTTON", L"Login", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            370, 232, 120, 32, hWnd, reinterpret_cast<HMENU>(IDC_LOGIN_BUTTON), g_instance, nullptr);

        g_controls.logoutButton = CreateWindowW(L"BUTTON", L"Logout", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            720, 46, 120, 30, hWnd, reinterpret_cast<HMENU>(IDC_LOGOUT_BUTTON), g_instance, nullptr);

        g_controls.activationLabel = CreateWindowW(L"STATIC", L"Activation code:", WS_CHILD | WS_VISIBLE,
            20, 305, 120, 20, hWnd, reinterpret_cast<HMENU>(IDC_ACTIVATION_LABEL), g_instance, nullptr);

        g_controls.activationEdit = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            150, 301, 260, 24, hWnd, reinterpret_cast<HMENU>(IDC_ACTIVATION_EDIT), g_instance, nullptr);

        g_controls.activateButton = CreateWindowW(L"BUTTON", L"Activate", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            430, 297, 120, 32, hWnd, reinterpret_cast<HMENU>(IDC_ACTIVATE_BUTTON), g_instance, nullptr);

        g_controls.scanFileLabel = CreateWindowW(L"STATIC", L"File:", WS_CHILD | WS_VISIBLE,
            20, 350, 120, 20, hWnd, reinterpret_cast<HMENU>(IDC_SCAN_FILE_LABEL), g_instance, nullptr);
        g_controls.scanFileEdit = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            150, 346, 520, 24, hWnd, reinterpret_cast<HMENU>(IDC_SCAN_FILE_EDIT), g_instance, nullptr);
        g_controls.scanFileButton = CreateWindowW(L"BUTTON", L"Scan file", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            690, 342, 160, 32, hWnd, reinterpret_cast<HMENU>(IDC_SCAN_FILE_BUTTON), g_instance, nullptr);

        g_controls.scanDirLabel = CreateWindowW(L"STATIC", L"Folder:", WS_CHILD | WS_VISIBLE,
            20, 390, 120, 20, hWnd, reinterpret_cast<HMENU>(IDC_SCAN_DIR_LABEL), g_instance, nullptr);
        g_controls.scanDirEdit = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            150, 386, 520, 24, hWnd, reinterpret_cast<HMENU>(IDC_SCAN_DIR_EDIT), g_instance, nullptr);
        g_controls.scanDirButton = CreateWindowW(L"BUTTON", L"Scan folder", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            690, 382, 160, 32, hWnd, reinterpret_cast<HMENU>(IDC_SCAN_DIR_BUTTON), g_instance, nullptr);

        g_controls.scanFixedButton = CreateWindowW(L"BUTTON", L"Scan fixed drives", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            20, 426, 260, 32, hWnd, reinterpret_cast<HMENU>(IDC_SCAN_FIXED_BUTTON), g_instance, nullptr);

        g_controls.scheduleLabel = CreateWindowW(L"STATIC", L"Schedule: path and interval, min.", WS_CHILD | WS_VISIBLE,
            20, 475, 260, 20, hWnd, reinterpret_cast<HMENU>(IDC_SCHEDULE_LABEL), g_instance, nullptr);
        g_controls.schedulePathEdit = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            20, 500, 430, 24, hWnd, reinterpret_cast<HMENU>(IDC_SCHEDULE_PATH_EDIT), g_instance, nullptr);
        g_controls.scheduleIntervalEdit = CreateWindowW(L"EDIT", L"60", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            460, 500, 80, 24, hWnd, reinterpret_cast<HMENU>(IDC_SCHEDULE_INTERVAL_EDIT), g_instance, nullptr);
        g_controls.scheduleEnableButton = CreateWindowW(L"BUTTON", L"Enable", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            550, 496, 100, 32, hWnd, reinterpret_cast<HMENU>(IDC_SCHEDULE_ENABLE_BUTTON), g_instance, nullptr);
        g_controls.scheduleDisableButton = CreateWindowW(L"BUTTON", L"Disable", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            660, 496, 100, 32, hWnd, reinterpret_cast<HMENU>(IDC_SCHEDULE_DISABLE_BUTTON), g_instance, nullptr);
        g_controls.scheduleResultButton = CreateWindowW(L"BUTTON", L"Result", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            770, 496, 100, 32, hWnd, reinterpret_cast<HMENU>(IDC_SCHEDULE_RESULT_BUTTON), g_instance, nullptr);

        g_controls.monitorLabel = CreateWindowW(L"STATIC", L"Directory monitoring:", WS_CHILD | WS_VISIBLE,
            20, 540, 260, 20, hWnd, reinterpret_cast<HMENU>(IDC_MONITOR_LABEL), g_instance, nullptr);
        g_controls.monitorDirEdit = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            20, 565, 520, 24, hWnd, reinterpret_cast<HMENU>(IDC_MONITOR_DIR_EDIT), g_instance, nullptr);
        g_controls.monitorEnableButton = CreateWindowW(L"BUTTON", L"Enable", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            550, 561, 100, 32, hWnd, reinterpret_cast<HMENU>(IDC_MONITOR_ENABLE_BUTTON), g_instance, nullptr);
        g_controls.monitorDisableButton = CreateWindowW(L"BUTTON", L"Disable", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            660, 561, 100, 32, hWnd, reinterpret_cast<HMENU>(IDC_MONITOR_DISABLE_BUTTON), g_instance, nullptr);
        g_controls.monitorResultButton = CreateWindowW(L"BUTTON", L"Result", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            770, 561, 100, 32, hWnd, reinterpret_cast<HMENU>(IDC_MONITOR_RESULT_BUTTON), g_instance, nullptr);

        g_controls.scanResultEdit = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
            20, 610, 850, 100, hWnd, reinterpret_cast<HMENU>(IDC_SCAN_RESULT_EDIT), g_instance, nullptr);

        UpdateWindowControls();
    }

    void RefreshStateFromService(bool setGenericErrorOnTransportFailure)
    {
        ServiceUserInfo userInfo;
        if (!GetCurrentUserInfo(userInfo))
        {
            g_state = {};
            g_state.blocked = true;
            if (setGenericErrorOnTransportFailure)
            {
                g_state.errorMessage = L"Failed to get service state.";
            }
            UpdateWindowControls();
            return;
        }

        if (userInfo.statusCode != ERROR_SUCCESS || !userInfo.authenticated)
        {
            g_state = {};
            g_state.blocked = true;
            UpdateWindowControls();
            return;
        }

        g_state.authenticated = true;
        g_state.username = userInfo.username;

        ServiceLicenseInfo licenseInfo;
        if (!GetCurrentLicenseInfo(licenseInfo))
        {
            g_state.hasLicense = false;
            g_state.blocked = true;
            g_state.expirationDate.clear();
            g_state.avDatabaseLoaded = false;
            if (setGenericErrorOnTransportFailure)
            {
                g_state.errorMessage = L"Failed to get license status.";
            }
            UpdateWindowControls();
            return;
        }

        if (licenseInfo.statusCode == ERROR_SUCCESS && licenseInfo.hasLicense)
        {
            g_state.hasLicense = true;
            g_state.blocked = licenseInfo.blocked;
            g_state.expirationDate = licenseInfo.expirationDate;
        }
        else if (licenseInfo.statusCode == ERROR_NOT_FOUND)
        {
            g_state.hasLicense = false;
            g_state.blocked = true;
            g_state.expirationDate.clear();
        }
        else if (licenseInfo.statusCode == ERROR_ACCESS_DENIED)
        {
            g_state = {};
            g_state.blocked = true;
        }
        else
        {
            g_state.hasLicense = false;
            g_state.blocked = true;
            g_state.expirationDate.clear();
            if (setGenericErrorOnTransportFailure)
            {
                g_state.errorMessage = L"Failed to update license information. Error code: " + std::to_wstring(licenseInfo.statusCode);
            }
        }

        if (g_state.authenticated && g_state.hasLicense && !g_state.blocked)
        {
            ServiceAvDatabaseInfo databaseInfo;
            if (GetAvDatabaseInfo(databaseInfo) && databaseInfo.statusCode == ERROR_SUCCESS)
            {
                g_state.avDatabaseLoaded = databaseInfo.loaded;
                g_state.avRecordCount = databaseInfo.recordCount;
                g_state.avReleaseDate = databaseInfo.releaseDate;
            }
            else
            {
                g_state.avDatabaseLoaded = false;
                g_state.avRecordCount = 0;
                g_state.avReleaseDate.clear();
            }
        }
        else
        {
            g_state.avDatabaseLoaded = false;
            g_state.avRecordCount = 0;
            g_state.avReleaseDate.clear();
        }

        UpdateWindowControls();
    }

    void StopServiceAndCloseWindow(HWND hWnd)
    {
        if (!ConfirmServiceStopOnSecureDesktop())
        {
            return;
        }

        if (!RequestServiceStop())
        {
            MessageBoxW(hWnd, L"Failed to stop Windows service via RPC.", L"Application", MB_OK | MB_ICONERROR);
            return;
        }

        DestroyWindow(hWnd);
    }

    void HandleLogin()
    {
        const std::wstring username = ReadWindowText(g_controls.loginEdit);
        const std::wstring password = ReadWindowText(g_controls.passwordEdit);

        DWORD statusCode = ERROR_GEN_FAILURE;
        std::wstring serverMessage;
        if (!RequestLogin(username, password, statusCode, serverMessage))
        {
            g_state = {};
            g_state.blocked = true;
            g_state.errorMessage = L"Failed to call Windows service via RPC.";
            UpdateWindowControls();
            return;
        }

        if (statusCode != ERROR_SUCCESS)
        {
            g_state = {};
            g_state.blocked = true;
            g_state.errorMessage = !serverMessage.empty() ? serverMessage : MapLoginStatusToMessage(statusCode);
            UpdateWindowControls();
            return;
        }

        g_state.errorMessage.clear();
        ClearEdit(g_controls.passwordEdit);
        RefreshStateFromService(true);
    }

    void HandleLogout()
    {
        DWORD statusCode = ERROR_GEN_FAILURE;
        if (!RequestLogout(statusCode) || statusCode != ERROR_SUCCESS)
        {
            g_state.errorMessage = L"Failed to log out.";
            UpdateWindowControls();
            return;
        }

        g_state = {};
        g_state.blocked = true;
        ClearEdit(g_controls.passwordEdit);
        ClearEdit(g_controls.activationEdit);
        SetScanResult(L"");
        UpdateWindowControls();
    }

    void HandleActivation()
    {
        const std::wstring activationCode = ReadWindowText(g_controls.activationEdit);

        DWORD statusCode = ERROR_GEN_FAILURE;
        std::wstring serverMessage;
        if (!RequestProductActivation(activationCode, statusCode, serverMessage))
        {
            g_state.hasLicense = false;
            g_state.blocked = true;
            g_state.expirationDate.clear();
            g_state.errorMessage = L"Failed to call Windows service via RPC.";
            UpdateWindowControls();
            return;
        }

        if (statusCode != ERROR_SUCCESS)
        {
            g_state.hasLicense = false;
            g_state.blocked = true;
            g_state.expirationDate.clear();
            g_state.errorMessage = !serverMessage.empty() ? serverMessage : MapActivationStatusToMessage(statusCode);
            UpdateWindowControls();
            return;
        }

        g_state.errorMessage.clear();
        RefreshStateFromService(true);
    }

    void HandleFileScan()
    {
        DWORD statusCode = ERROR_GEN_FAILURE;
        std::wstring result;
        if (!RequestFileScan(ReadWindowText(g_controls.scanFileEdit), statusCode, result))
        {
            SetScanResult(L"Failed to call Windows service via RPC.");
            return;
        }
        SetScanResult(statusCode == ERROR_SUCCESS ? result : (result + L"\r\n" + MapScanStatusToMessage(statusCode)));
    }

    void HandleDirectoryScan()
    {
        DWORD statusCode = ERROR_GEN_FAILURE;
        std::wstring result;
        if (!RequestDirectoryScan(ReadWindowText(g_controls.scanDirEdit), statusCode, result))
        {
            SetScanResult(L"Failed to call Windows service via RPC.");
            return;
        }
        SetScanResult(statusCode == ERROR_SUCCESS ? result : (result + L"\r\n" + MapScanStatusToMessage(statusCode)));
    }

    void HandleFixedDrivesScan()
    {
        DWORD statusCode = ERROR_GEN_FAILURE;
        std::wstring result;
        if (!RequestFixedDrivesScan(statusCode, result))
        {
            SetScanResult(L"Failed to call Windows service via RPC.");
            return;
        }
        SetScanResult(statusCode == ERROR_SUCCESS ? result : (result + L"\r\n" + MapScanStatusToMessage(statusCode)));
    }

    void HandleScheduleEnable()
    {
        DWORD statusCode = ERROR_GEN_FAILURE;
        std::wstring result;
        const long interval = ReadLongFromEdit(g_controls.scheduleIntervalEdit);
        if (!ConfigureScheduledScan(true, ReadWindowText(g_controls.schedulePathEdit), interval, statusCode, result))
        {
            SetScanResult(L"Failed to call Windows service via RPC.");
            return;
        }
        SetScanResult(statusCode == ERROR_SUCCESS ? result : (result + L"\r\n" + MapScanStatusToMessage(statusCode)));
    }

    void HandleScheduleDisable()
    {
        DWORD statusCode = ERROR_GEN_FAILURE;
        std::wstring result;
        if (!ConfigureScheduledScan(false, L"", 0, statusCode, result))
        {
            SetScanResult(L"Failed to call Windows service via RPC.");
            return;
        }
        SetScanResult(result);
    }

    void HandleScheduleResult()
    {
        DWORD statusCode = ERROR_GEN_FAILURE;
        std::wstring result;
        if (!GetScheduledScanResult(statusCode, result))
        {
            SetScanResult(L"Failed to call Windows service via RPC.");
            return;
        }
        SetScanResult(result);
    }

    void HandleMonitorEnable()
    {
        DWORD statusCode = ERROR_GEN_FAILURE;
        std::wstring result;
        if (!ConfigureMonitoredDirectory(true, ReadWindowText(g_controls.monitorDirEdit), statusCode, result))
        {
            SetScanResult(L"Failed to call Windows service via RPC.");
            return;
        }
        SetScanResult(statusCode == ERROR_SUCCESS ? result : (result + L"\r\n" + MapScanStatusToMessage(statusCode)));
    }

    void HandleMonitorDisable()
    {
        DWORD statusCode = ERROR_GEN_FAILURE;
        std::wstring result;
        if (!ConfigureMonitoredDirectory(false, L"", statusCode, result))
        {
            SetScanResult(L"Failed to call Windows service via RPC.");
            return;
        }
        SetScanResult(result);
    }

    void HandleMonitorResult()
    {
        DWORD statusCode = ERROR_GEN_FAILURE;
        std::wstring result;
        if (!GetMonitorResult(statusCode, result))
        {
            SetScanResult(L"Failed to call Windows service via RPC.");
            return;
        }
        SetScanResult(result);
    }
}

ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(WNDCLASSEXW);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = hInstance;
    windowClass.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszMenuName = MAKEINTRESOURCEW(IDC_APPLICATION);
    windowClass.lpszClassName = g_windowClass;
    windowClass.hIconSm = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON_SMALL));
    return RegisterClassExW(&windowClass);
}

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow, LPCWSTR lpCmdLine)
{
    g_instance = hInstance;

    g_mainWindow = CreateWindowW(
        g_windowClass,
        g_windowTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        0,
        920,
        790,
        nullptr,
        nullptr,
        hInstance,
        nullptr);

    if (g_mainWindow == nullptr)
    {
        return FALSE;
    }

    CreateControls(g_mainWindow);
    AddTrayIcon(g_mainWindow);
    SetTimer(g_mainWindow, kStatePollingTimerId, kStatePollingIntervalMs, nullptr);
    RefreshStateFromService(true);

    if (!ShouldStartHidden(lpCmdLine))
    {
        ShowWindow(g_mainWindow, nCmdShow);
        UpdateWindow(g_mainWindow);
    }

    return TRUE;
}

int APIENTRY wWinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine,
    _In_ int nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);

    if (IsSecureStopCommandLine(lpCmdLine))
    {
        return RequestServiceStop() ? 0 : 1;
    }

    if (IsSecureStartCommandLine(lpCmdLine))
    {
        return EnsureServiceRunningAndWait(kServiceName, 30000) ? 0 : 1;
    }

    SaveApplicationPathForService();

    if (!IsStartedByService(kServiceName))
    {
        if (!RequestServiceStart())
        {
            MessageBoxW(nullptr, L"Failed to start Windows service or wait for Running state.", L"Application", MB_OK | MB_ICONERROR);
        }

        return 0;
    }

    ProtectCurrentProcessFromUserTermination();

    if (!CreateApplicationMutex())
    {
        return 0;
    }

    LoadStringW(hInstance, IDS_APP_TITLE, g_windowTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_APPLICATION, g_windowClass, MAX_LOADSTRING);

    MyRegisterClass(hInstance);

    if (!InitInstance(hInstance, nCmdShow, lpCmdLine))
    {
        CloseApplicationMutex();
        return 0;
    }

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return static_cast<int>(message.wParam);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == GetTaskbarCreatedMessage())
    {
        AddTrayIcon(hWnd);
        return 0;
    }

    switch (message)
    {
    case WM_COMMAND:
    {
        const int controlId = LOWORD(wParam);

        if (controlId == IDM_EXIT || IDM_TRAY_EXIT == controlId)
        {
            StopServiceAndCloseWindow(hWnd);
            return 0;
        }

        if (controlId == IDM_TRAY_OPEN)
        {
            ShowMainWindowFromTray(hWnd);
            return 0;
        }

        if (controlId == IDC_LOGIN_BUTTON)
        {
            HandleLogin();
            return 0;
        }

        if (controlId == IDC_LOGOUT_BUTTON)
        {
            HandleLogout();
            return 0;
        }

        if (controlId == IDC_ACTIVATE_BUTTON)
        {
            HandleActivation();
            return 0;
        }

        if (controlId == IDC_SCAN_FILE_BUTTON)
        {
            HandleFileScan();
            return 0;
        }

        if (controlId == IDC_SCAN_DIR_BUTTON)
        {
            HandleDirectoryScan();
            return 0;
        }

        if (controlId == IDC_SCAN_FIXED_BUTTON)
        {
            HandleFixedDrivesScan();
            return 0;
        }

        if (controlId == IDC_SCHEDULE_ENABLE_BUTTON)
        {
            HandleScheduleEnable();
            return 0;
        }

        if (controlId == IDC_SCHEDULE_DISABLE_BUTTON)
        {
            HandleScheduleDisable();
            return 0;
        }

        if (controlId == IDC_SCHEDULE_RESULT_BUTTON)
        {
            HandleScheduleResult();
            return 0;
        }

        if (controlId == IDC_MONITOR_ENABLE_BUTTON)
        {
            HandleMonitorEnable();
            return 0;
        }

        if (controlId == IDC_MONITOR_DISABLE_BUTTON)
        {
            HandleMonitorDisable();
            return 0;
        }

        if (controlId == IDC_MONITOR_RESULT_BUTTON)
        {
            HandleMonitorResult();
            return 0;
        }

        return DefWindowProcW(hWnd, message, wParam, lParam);
    }

    case WM_TIMER:
        if (wParam == kStatePollingTimerId)
        {
            RefreshStateFromService(false);
            return 0;
        }
        break;

    case WM_CLOSE:
        HideMainWindowToTray(hWnd);
        return 0;

    case WM_TRAYICON:
        if (lParam == WM_LBUTTONUP)
        {
            ShowMainWindowFromTray(hWnd);
            return 0;
        }

        if (lParam == WM_RBUTTONUP)
        {
            ShowTrayMenu(hWnd);
            return 0;
        }
        break;

    case WM_PAINT:
    {
        PAINTSTRUCT paintStruct;
        HDC deviceContext = BeginPaint(hWnd, &paintStruct);
        UNREFERENCED_PARAMETER(deviceContext);
        EndPaint(hWnd, &paintStruct);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hWnd, kStatePollingTimerId);
        RemoveTrayIcon(hWnd);
        CloseApplicationMutex();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hWnd, message, wParam, lParam);
}