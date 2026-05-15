#include "framework.h"
#include "ServiceControlClient.h"
#include "ServiceControlRpc_h.h"

#include <TlHelp32.h>
#include <Aclapi.h>
#include <Shellapi.h>
#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Rpcrt4.lib")
#pragma comment(lib, "Shell32.lib")

void* __RPC_USER MIDL_user_allocate(size_t size)
{
    return malloc(size);
}

void __RPC_USER MIDL_user_free(void* pointer)
{
    free(pointer);
}

extern "C"
{
    long RpcGetCurrentUser(handle_t bindingHandle, long* isAuthenticated, long userNameBufferLength, wchar_t* userNameBuffer);
    long RpcLogin(handle_t bindingHandle, wchar_t* username, wchar_t* password, long errorMessageBufferLength, wchar_t* errorMessageBuffer);
    long RpcLogout(handle_t bindingHandle);
    long RpcGetLicenseInfo(handle_t bindingHandle, long* hasLicense, long* isBlocked, long expirationDateBufferLength, wchar_t* expirationDateBuffer);
    long RpcActivateProduct(handle_t bindingHandle, wchar_t* activationCode, long errorMessageBufferLength, wchar_t* errorMessageBuffer);
    long RpcGetAvDatabaseInfo(handle_t bindingHandle, long* isLoaded, long* recordCount, long releaseDateBufferLength, wchar_t* releaseDateBuffer);
    long RpcScanFile(handle_t bindingHandle, wchar_t* filePath, long resultBufferLength, wchar_t* resultBuffer);
    long RpcScanDirectory(handle_t bindingHandle, wchar_t* directoryPath, long resultBufferLength, wchar_t* resultBuffer);
    long RpcScanFixedDrives(handle_t bindingHandle, long resultBufferLength, wchar_t* resultBuffer);
    long RpcSetScheduledScan(handle_t bindingHandle, long enabled, wchar_t* path, long intervalMinutes, long resultBufferLength, wchar_t* resultBuffer);
    long RpcGetScheduledScanResult(handle_t bindingHandle, long resultBufferLength, wchar_t* resultBuffer);
    long RpcSetMonitoredDirectory(handle_t bindingHandle, long enabled, wchar_t* directoryPath, long resultBufferLength, wchar_t* resultBuffer);
    long RpcGetMonitorResult(handle_t bindingHandle, long resultBufferLength, wchar_t* resultBuffer);
    void RpcStopService(handle_t bindingHandle);
}

namespace
{
    bool QueryServiceStatusProcess(SC_HANDLE serviceHandle, SERVICE_STATUS_PROCESS& status)
    {
        DWORD bytesNeeded = 0;
        ZeroMemory(&status, sizeof(status));

        return QueryServiceStatusEx(
            serviceHandle,
            SC_STATUS_PROCESS_INFO,
            reinterpret_cast<LPBYTE>(&status),
            sizeof(status),
            &bytesNeeded) != FALSE;
    }

    bool WaitForRunningState(SC_HANDLE serviceHandle, DWORD timeoutMs)
    {
        const DWORD startTick = GetTickCount();

        while (true)
        {
            SERVICE_STATUS_PROCESS status = {};
            if (!QueryServiceStatusProcess(serviceHandle, status))
            {
                return false;
            }

            if (status.dwCurrentState == SERVICE_RUNNING)
            {
                return true;
            }

            if (status.dwCurrentState == SERVICE_STOPPED)
            {
                return false;
            }

            if (GetTickCount() - startTick > timeoutMs)
            {
                return false;
            }

            Sleep(500);
        }
    }

    bool QueryServiceCurrentState(const wchar_t* serviceName, DWORD& currentState)
    {
        currentState = SERVICE_STOPPED;

        SC_HANDLE scmHandle = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (scmHandle == nullptr)
        {
            return false;
        }

        SC_HANDLE serviceHandle = OpenServiceW(scmHandle, serviceName, SERVICE_QUERY_STATUS);
        if (serviceHandle == nullptr)
        {
            CloseServiceHandle(scmHandle);
            return false;
        }

        SERVICE_STATUS_PROCESS status = {};
        const bool success = QueryServiceStatusProcess(serviceHandle, status);

        CloseServiceHandle(serviceHandle);
        CloseServiceHandle(scmHandle);

        if (!success)
        {
            return false;
        }

        currentState = status.dwCurrentState;
        return true;
    }

    DWORD QueryServiceProcessId(const wchar_t* serviceName)
    {
        SC_HANDLE scmHandle = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (scmHandle == nullptr)
        {
            return 0;
        }

        SC_HANDLE serviceHandle = OpenServiceW(scmHandle, serviceName, SERVICE_QUERY_STATUS);
        if (serviceHandle == nullptr)
        {
            CloseServiceHandle(scmHandle);
            return 0;
        }

        SERVICE_STATUS_PROCESS status = {};
        const bool success = QueryServiceStatusProcess(serviceHandle, status);

        CloseServiceHandle(serviceHandle);
        CloseServiceHandle(scmHandle);

        if (!success)
        {
            return 0;
        }

        return status.dwProcessId;
    }

    DWORD GetParentProcessId()
    {
        const DWORD currentProcessId = GetCurrentProcessId();
        HANDLE snapshotHandle = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshotHandle == INVALID_HANDLE_VALUE)
        {
            return 0;
        }

        PROCESSENTRY32W entry = {};
        entry.dwSize = sizeof(entry);

        DWORD parentProcessId = 0;

        if (Process32FirstW(snapshotHandle, &entry))
        {
            do
            {
                if (entry.th32ProcessID == currentProcessId)
                {
                    parentProcessId = entry.th32ParentProcessID;
                    break;
                }
            } while (Process32NextW(snapshotHandle, &entry));
        }

        CloseHandle(snapshotHandle);
        return parentProcessId;
    }

    std::wstring ExpandPath(const wchar_t* value)
    {
        DWORD required = ExpandEnvironmentStringsW(value, nullptr, 0);
        if (required == 0)
        {
            return L"";
        }

        std::wstring expanded(required, L'\0');
        ExpandEnvironmentStringsW(value, expanded.data(), required);

        while (!expanded.empty() && expanded.back() == L'\0')
        {
            expanded.pop_back();
        }

        return expanded;
    }

    bool CreateServiceBinding(RPC_BINDING_HANDLE& bindingHandle)
    {
        RPC_WSTR stringBinding = nullptr;
        bindingHandle = nullptr;

        RPC_STATUS status = RpcStringBindingComposeW(
            nullptr,
            reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(kRpcProtocolSequence)),
            nullptr,
            reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(kRpcEndpoint)),
            nullptr,
            &stringBinding);

        if (status != RPC_S_OK)
        {
            return false;
        }

        status = RpcBindingFromStringBindingW(stringBinding, &bindingHandle);
        RpcStringFreeW(&stringBinding);

        return status == RPC_S_OK;
    }

    std::vector<wchar_t> MakeMutableStringBuffer(const std::wstring& value)
    {
        std::vector<wchar_t> buffer(value.begin(), value.end());
        buffer.push_back(L'\0');
        return buffer;
    }

    bool RpcCallGetCurrentUser(RPC_BINDING_HANDLE bindingHandle, long* isAuthenticated, long userNameBufferLength, wchar_t* userNameBuffer, long* statusCode)
    {
        long localStatus = ERROR_GEN_FAILURE;
        bool success = false;

        RpcTryExcept
        {
            localStatus = RpcGetCurrentUser(bindingHandle, isAuthenticated, userNameBufferLength, userNameBuffer);
            success = true;
        }
            RpcExcept(1)
        {
            success = false;
        }
        RpcEndExcept;

        if (statusCode != nullptr)
        {
            *statusCode = localStatus;
        }

        return success;
    }

    bool RpcCallLogin(RPC_BINDING_HANDLE bindingHandle,
        wchar_t* username,
        wchar_t* password,
        long errorMessageBufferLength,
        wchar_t* errorMessageBuffer,
        long* statusCode)
    {
        long localStatus = ERROR_GEN_FAILURE;
        bool success = false;

        RpcTryExcept
        {
            localStatus = RpcLogin(bindingHandle, username, password, errorMessageBufferLength, errorMessageBuffer);
            success = true;
        }
            RpcExcept(1)
        {
            success = false;
        }
        RpcEndExcept;

        if (statusCode != nullptr)
        {
            *statusCode = localStatus;
        }

        return success;
    }

    bool RpcCallLogout(RPC_BINDING_HANDLE bindingHandle, long* statusCode)
    {
        long localStatus = ERROR_GEN_FAILURE;
        bool success = false;

        RpcTryExcept
        {
            localStatus = RpcLogout(bindingHandle);
            success = true;
        }
            RpcExcept(1)
        {
            success = false;
        }
        RpcEndExcept;

        if (statusCode != nullptr)
        {
            *statusCode = localStatus;
        }

        return success;
    }

    bool RpcCallGetLicenseInfo(RPC_BINDING_HANDLE bindingHandle, long* hasLicense, long* isBlocked, long expirationDateBufferLength, wchar_t* expirationDateBuffer, long* statusCode)
    {
        long localStatus = ERROR_GEN_FAILURE;
        bool success = false;

        RpcTryExcept
        {
            localStatus = RpcGetLicenseInfo(bindingHandle, hasLicense, isBlocked, expirationDateBufferLength, expirationDateBuffer);
            success = true;
        }
            RpcExcept(1)
        {
            success = false;
        }
        RpcEndExcept;

        if (statusCode != nullptr)
        {
            *statusCode = localStatus;
        }

        return success;
    }

    bool RpcCallActivateProduct(RPC_BINDING_HANDLE bindingHandle,
        wchar_t* activationCode,
        long errorMessageBufferLength,
        wchar_t* errorMessageBuffer,
        long* statusCode)
    {
        long localStatus = ERROR_GEN_FAILURE;
        bool success = false;

        RpcTryExcept
        {
            localStatus = RpcActivateProduct(bindingHandle, activationCode, errorMessageBufferLength, errorMessageBuffer);
            success = true;
        }
            RpcExcept(1)
        {
            success = false;
        }
        RpcEndExcept;

        if (statusCode != nullptr)
        {
            *statusCode = localStatus;
        }

        return success;
    }

    bool RpcCallGetAvDatabaseInfo(RPC_BINDING_HANDLE bindingHandle, long* isLoaded, long* recordCount, long releaseDateBufferLength, wchar_t* releaseDateBuffer, long* statusCode)
    {
        long localStatus = ERROR_GEN_FAILURE;
        bool success = false;
        RpcTryExcept
        {
            localStatus = RpcGetAvDatabaseInfo(bindingHandle, isLoaded, recordCount, releaseDateBufferLength, releaseDateBuffer);
            success = true;
        }
            RpcExcept(1)
        {
            success = false;
        }
        RpcEndExcept;
        if (statusCode != nullptr) *statusCode = localStatus;
        return success;
    }

    bool RpcCallScanFile(RPC_BINDING_HANDLE bindingHandle, wchar_t* filePath, long resultBufferLength, wchar_t* resultBuffer, long* statusCode)
    {
        long localStatus = ERROR_GEN_FAILURE;
        bool success = false;
        RpcTryExcept
        {
            localStatus = RpcScanFile(bindingHandle, filePath, resultBufferLength, resultBuffer);
            success = true;
        }
            RpcExcept(1)
        {
            success = false;
        }
        RpcEndExcept;
        if (statusCode != nullptr) *statusCode = localStatus;
        return success;
    }

    bool RpcCallScanDirectory(RPC_BINDING_HANDLE bindingHandle, wchar_t* directoryPath, long resultBufferLength, wchar_t* resultBuffer, long* statusCode)
    {
        long localStatus = ERROR_GEN_FAILURE;
        bool success = false;
        RpcTryExcept
        {
            localStatus = RpcScanDirectory(bindingHandle, directoryPath, resultBufferLength, resultBuffer);
            success = true;
        }
            RpcExcept(1)
        {
            success = false;
        }
        RpcEndExcept;
        if (statusCode != nullptr) *statusCode = localStatus;
        return success;
    }

    bool RpcCallScanFixedDrives(RPC_BINDING_HANDLE bindingHandle, long resultBufferLength, wchar_t* resultBuffer, long* statusCode)
    {
        long localStatus = ERROR_GEN_FAILURE;
        bool success = false;
        RpcTryExcept
        {
            localStatus = RpcScanFixedDrives(bindingHandle, resultBufferLength, resultBuffer);
            success = true;
        }
            RpcExcept(1)
        {
            success = false;
        }
        RpcEndExcept;
        if (statusCode != nullptr) *statusCode = localStatus;
        return success;
    }

    bool RpcCallSetScheduledScan(RPC_BINDING_HANDLE bindingHandle, long enabled, wchar_t* path, long intervalMinutes, long resultBufferLength, wchar_t* resultBuffer, long* statusCode)
    {
        long localStatus = ERROR_GEN_FAILURE;
        bool success = false;
        RpcTryExcept
        {
            localStatus = RpcSetScheduledScan(bindingHandle, enabled, path, intervalMinutes, resultBufferLength, resultBuffer);
            success = true;
        }
            RpcExcept(1)
        {
            success = false;
        }
        RpcEndExcept;
        if (statusCode != nullptr) *statusCode = localStatus;
        return success;
    }

    bool RpcCallGetScheduledScanResult(RPC_BINDING_HANDLE bindingHandle, long resultBufferLength, wchar_t* resultBuffer, long* statusCode)
    {
        long localStatus = ERROR_GEN_FAILURE;
        bool success = false;
        RpcTryExcept
        {
            localStatus = RpcGetScheduledScanResult(bindingHandle, resultBufferLength, resultBuffer);
            success = true;
        }
            RpcExcept(1)
        {
            success = false;
        }
        RpcEndExcept;
        if (statusCode != nullptr) *statusCode = localStatus;
        return success;
    }

    bool RpcCallSetMonitoredDirectory(RPC_BINDING_HANDLE bindingHandle, long enabled, wchar_t* directoryPath, long resultBufferLength, wchar_t* resultBuffer, long* statusCode)
    {
        long localStatus = ERROR_GEN_FAILURE;
        bool success = false;
        RpcTryExcept
        {
            localStatus = RpcSetMonitoredDirectory(bindingHandle, enabled, directoryPath, resultBufferLength, resultBuffer);
            success = true;
        }
            RpcExcept(1)
        {
            success = false;
        }
        RpcEndExcept;
        if (statusCode != nullptr) *statusCode = localStatus;
        return success;
    }

    bool RpcCallGetMonitorResult(RPC_BINDING_HANDLE bindingHandle, long resultBufferLength, wchar_t* resultBuffer, long* statusCode)
    {
        long localStatus = ERROR_GEN_FAILURE;
        bool success = false;
        RpcTryExcept
        {
            localStatus = RpcGetMonitorResult(bindingHandle, resultBufferLength, resultBuffer);
            success = true;
        }
            RpcExcept(1)
        {
            success = false;
        }
        RpcEndExcept;
        if (statusCode != nullptr) *statusCode = localStatus;
        return success;
    }

    bool RequestServiceStopThroughRpc()
    {
        RPC_BINDING_HANDLE bindingHandle = nullptr;
        bool success = false;

        if (!CreateServiceBinding(bindingHandle))
        {
            return false;
        }

        RpcTryExcept
        {
            RpcStopService(bindingHandle);
            success = true;
        }
            RpcExcept(1)
        {
            success = false;
        }
        RpcEndExcept;

        RpcBindingFree(&bindingHandle);
        return success;
    }

    bool IsCurrentProcessElevated()
    {
        HANDLE tokenHandle = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tokenHandle))
        {
            return false;
        }

        TOKEN_ELEVATION elevation = {};
        DWORD returnedLength = 0;
        const BOOL ok = GetTokenInformation(tokenHandle, TokenElevation, &elevation, sizeof(elevation), &returnedLength);
        CloseHandle(tokenHandle);

        return ok && elevation.TokenIsElevated != 0;
    }

    std::wstring GetCurrentExecutablePath()
    {
        std::wstring path(MAX_PATH, L'\0');
        DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        while (length == path.size())
        {
            path.resize(path.size() * 2);
            length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        }

        if (length == 0)
        {
            return L"";
        }

        path.resize(length);
        return path;
    }

    bool RunElevatedCommand(const wchar_t* commandLineParameter)
    {
        const std::wstring executablePath = GetCurrentExecutablePath();
        if (executablePath.empty())
        {
            return false;
        }

        SHELLEXECUTEINFOW executeInfo = {};
        executeInfo.cbSize = sizeof(executeInfo);
        executeInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
        executeInfo.hwnd = nullptr;
        executeInfo.lpVerb = L"runas";
        executeInfo.lpFile = executablePath.c_str();
        executeInfo.lpParameters = commandLineParameter;
        executeInfo.nShow = SW_HIDE;

        if (!ShellExecuteExW(&executeInfo) || executeInfo.hProcess == nullptr)
        {
            return false;
        }

        WaitForSingleObject(executeInfo.hProcess, INFINITE);

        DWORD exitCode = 1;
        GetExitCodeProcess(executeInfo.hProcess, &exitCode);
        CloseHandle(executeInfo.hProcess);

        return exitCode == 0;
    }

    bool RunElevatedSecureStop()
    {
        return RunElevatedCommand(L"--secure-stop");
    }

    bool RunElevatedSecureStart()
    {
        return RunElevatedCommand(L"--secure-start");
    }
}

bool EnsureServiceRunningAndWait(const wchar_t* serviceName, DWORD timeoutMs)
{
    SC_HANDLE scmHandle = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scmHandle == nullptr)
    {
        return false;
    }

    SC_HANDLE serviceHandle = OpenServiceW(
        scmHandle,
        serviceName,
        SERVICE_QUERY_STATUS | SERVICE_START);

    if (serviceHandle == nullptr)
    {
        CloseServiceHandle(scmHandle);
        return false;
    }

    SERVICE_STATUS_PROCESS status = {};
    if (!QueryServiceStatusProcess(serviceHandle, status))
    {
        CloseServiceHandle(serviceHandle);
        CloseServiceHandle(scmHandle);
        return false;
    }

    if (status.dwCurrentState == SERVICE_RUNNING)
    {
        CloseServiceHandle(serviceHandle);
        CloseServiceHandle(scmHandle);
        return true;
    }

    if (status.dwCurrentState == SERVICE_STOPPED)
    {
        if (!StartServiceW(serviceHandle, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING)
        {
            CloseServiceHandle(serviceHandle);
            CloseServiceHandle(scmHandle);
            return false;
        }
    }

    const bool result = WaitForRunningState(serviceHandle, timeoutMs);

    CloseServiceHandle(serviceHandle);
    CloseServiceHandle(scmHandle);

    return result;
}

bool RequestServiceStart()
{
    DWORD currentState = SERVICE_STOPPED;
    if (QueryServiceCurrentState(kServiceName, currentState) && currentState == SERVICE_RUNNING)
    {
        return true;
    }

    if (IsCurrentProcessElevated())
    {
        return EnsureServiceRunningAndWait(kServiceName, 30000);
    }

    if (!RunElevatedSecureStart())
    {
        return false;
    }

    return QueryServiceCurrentState(kServiceName, currentState) && currentState == SERVICE_RUNNING;
}

bool IsStartedByService(const wchar_t* serviceName)
{
    const DWORD parentProcessId = GetParentProcessId();
    const DWORD serviceProcessId = QueryServiceProcessId(serviceName);

    return parentProcessId != 0 && serviceProcessId != 0 && parentProcessId == serviceProcessId;
}

bool RequestServiceStop()
{
    if (IsCurrentProcessElevated())
    {
        return RequestServiceStopThroughRpc();
    }

    return RunElevatedSecureStop();
}

bool ProtectCurrentProcessFromUserTermination()
{
    SID_IDENTIFIER_AUTHORITY worldAuthority = SECURITY_WORLD_SID_AUTHORITY;
    PSID everyoneSid = nullptr;

    if (!AllocateAndInitializeSid(&worldAuthority,
        1,
        SECURITY_WORLD_RID,
        0, 0, 0, 0, 0, 0, 0,
        &everyoneSid))
    {
        return false;
    }

    PSECURITY_DESCRIPTOR securityDescriptor = nullptr;
    PACL oldDacl = nullptr;
    PACL newDacl = nullptr;

    DWORD status = GetSecurityInfo(GetCurrentProcess(),
        SE_KERNEL_OBJECT,
        DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        &oldDacl,
        nullptr,
        &securityDescriptor);

    if (status == ERROR_SUCCESS)
    {
        EXPLICIT_ACCESSW denyEntry = {};
        denyEntry.grfAccessPermissions = PROCESS_TERMINATE;
        denyEntry.grfAccessMode = DENY_ACCESS;
        denyEntry.grfInheritance = NO_INHERITANCE;
        BuildTrusteeWithSidW(&denyEntry.Trustee, everyoneSid);

        status = SetEntriesInAclW(1, &denyEntry, oldDacl, &newDacl);
        if (status == ERROR_SUCCESS)
        {
            status = SetSecurityInfo(GetCurrentProcess(),
                SE_KERNEL_OBJECT,
                DACL_SECURITY_INFORMATION,
                nullptr,
                nullptr,
                newDacl,
                nullptr);
        }
    }

    if (newDacl != nullptr)
    {
        LocalFree(newDacl);
    }
    if (securityDescriptor != nullptr)
    {
        LocalFree(securityDescriptor);
    }
    if (everyoneSid != nullptr)
    {
        FreeSid(everyoneSid);
    }

    return status == ERROR_SUCCESS;
}

bool IsSecureStopCommandLine(LPCWSTR commandLine)
{
    return commandLine != nullptr && wcsstr(commandLine, L"--secure-stop") != nullptr;
}

bool IsSecureStartCommandLine(LPCWSTR commandLine)
{
    return commandLine != nullptr && wcsstr(commandLine, L"--secure-start") != nullptr;
}

bool GetCurrentUserInfo(ServiceUserInfo& info)
{
    info = {};

    RPC_BINDING_HANDLE bindingHandle = nullptr;
    if (!CreateServiceBinding(bindingHandle))
    {
        return false;
    }

    wchar_t userNameBuffer[256] = {};
    long authenticated = 0;
    long statusCode = ERROR_GEN_FAILURE;

    const bool success = RpcCallGetCurrentUser(bindingHandle,
        &authenticated,
        static_cast<long>(ARRAYSIZE(userNameBuffer)),
        userNameBuffer,
        &statusCode);

    RpcBindingFree(&bindingHandle);

    if (!success)
    {
        return false;
    }

    info.statusCode = static_cast<DWORD>(statusCode);
    info.authenticated = authenticated != 0;
    info.username = userNameBuffer;
    return true;
}

bool RequestLogin(const std::wstring& username, const std::wstring& password, DWORD& statusCode, std::wstring& errorMessage)
{
    statusCode = ERROR_GEN_FAILURE;
    errorMessage.clear();

    RPC_BINDING_HANDLE bindingHandle = nullptr;
    if (!CreateServiceBinding(bindingHandle))
    {
        return false;
    }

    std::vector<wchar_t> userBuffer = MakeMutableStringBuffer(username);
    std::vector<wchar_t> passwordBuffer = MakeMutableStringBuffer(password);
    wchar_t errorBuffer[2048] = {};

    long rpcStatus = ERROR_GEN_FAILURE;
    const bool success = RpcCallLogin(bindingHandle,
        userBuffer.data(),
        passwordBuffer.data(),
        static_cast<long>(ARRAYSIZE(errorBuffer)),
        errorBuffer,
        &rpcStatus);

    RpcBindingFree(&bindingHandle);

    statusCode = static_cast<DWORD>(rpcStatus);
    errorMessage = errorBuffer;
    return success;
}

bool RequestLogout(DWORD& statusCode)
{
    statusCode = ERROR_GEN_FAILURE;

    RPC_BINDING_HANDLE bindingHandle = nullptr;
    if (!CreateServiceBinding(bindingHandle))
    {
        return false;
    }

    long rpcStatus = ERROR_GEN_FAILURE;
    const bool success = RpcCallLogout(bindingHandle, &rpcStatus);

    RpcBindingFree(&bindingHandle);

    statusCode = static_cast<DWORD>(rpcStatus);
    return success;
}

bool GetCurrentLicenseInfo(ServiceLicenseInfo& info)
{
    info = {};

    RPC_BINDING_HANDLE bindingHandle = nullptr;
    if (!CreateServiceBinding(bindingHandle))
    {
        return false;
    }

    wchar_t expirationBuffer[128] = {};
    long hasLicense = 0;
    long isBlocked = 1;
    long statusCode = ERROR_GEN_FAILURE;

    const bool success = RpcCallGetLicenseInfo(bindingHandle,
        &hasLicense,
        &isBlocked,
        static_cast<long>(ARRAYSIZE(expirationBuffer)),
        expirationBuffer,
        &statusCode);

    RpcBindingFree(&bindingHandle);

    if (!success)
    {
        return false;
    }

    info.statusCode = static_cast<DWORD>(statusCode);
    info.hasLicense = hasLicense != 0;
    info.blocked = isBlocked != 0;
    info.expirationDate = expirationBuffer;
    return true;
}

bool RequestProductActivation(const std::wstring& activationCode, DWORD& statusCode, std::wstring& errorMessage)
{
    statusCode = ERROR_GEN_FAILURE;
    errorMessage.clear();

    RPC_BINDING_HANDLE bindingHandle = nullptr;
    if (!CreateServiceBinding(bindingHandle))
    {
        return false;
    }

    std::vector<wchar_t> activationBuffer = MakeMutableStringBuffer(activationCode);
    wchar_t errorBuffer[2048] = {};

    long rpcStatus = ERROR_GEN_FAILURE;
    const bool success = RpcCallActivateProduct(bindingHandle,
        activationBuffer.data(),
        static_cast<long>(ARRAYSIZE(errorBuffer)),
        errorBuffer,
        &rpcStatus);

    RpcBindingFree(&bindingHandle);

    statusCode = static_cast<DWORD>(rpcStatus);
    errorMessage = errorBuffer;
    return success;
}

bool GetAvDatabaseInfo(ServiceAvDatabaseInfo& info)
{
    info = {};

    RPC_BINDING_HANDLE bindingHandle = nullptr;
    if (!CreateServiceBinding(bindingHandle))
    {
        return false;
    }

    wchar_t releaseDateBuffer[256] = {};
    long isLoaded = 0;
    long recordCount = 0;
    long statusCode = ERROR_GEN_FAILURE;

    const bool success = RpcCallGetAvDatabaseInfo(bindingHandle,
        &isLoaded,
        &recordCount,
        static_cast<long>(ARRAYSIZE(releaseDateBuffer)),
        releaseDateBuffer,
        &statusCode);

    RpcBindingFree(&bindingHandle);

    if (!success)
    {
        return false;
    }

    info.statusCode = static_cast<DWORD>(statusCode);
    info.loaded = isLoaded != 0;
    info.recordCount = recordCount;
    info.releaseDate = releaseDateBuffer;
    return true;
}

bool RequestFileScan(const std::wstring& filePath, DWORD& statusCode, std::wstring& resultText)
{
    statusCode = ERROR_GEN_FAILURE;
    resultText.clear();

    RPC_BINDING_HANDLE bindingHandle = nullptr;
    if (!CreateServiceBinding(bindingHandle))
    {
        return false;
    }

    std::vector<wchar_t> pathBuffer = MakeMutableStringBuffer(filePath);
    std::vector<wchar_t> resultBuffer(65536, L'\0');
    long rpcStatus = ERROR_GEN_FAILURE;

    const bool success = RpcCallScanFile(bindingHandle,
        pathBuffer.data(),
        static_cast<long>(resultBuffer.size()),
        resultBuffer.data(),
        &rpcStatus);

    RpcBindingFree(&bindingHandle);
    statusCode = static_cast<DWORD>(rpcStatus);
    resultText = resultBuffer.data();
    return success;
}

bool RequestDirectoryScan(const std::wstring& directoryPath, DWORD& statusCode, std::wstring& resultText)
{
    statusCode = ERROR_GEN_FAILURE;
    resultText.clear();

    RPC_BINDING_HANDLE bindingHandle = nullptr;
    if (!CreateServiceBinding(bindingHandle))
    {
        return false;
    }

    std::vector<wchar_t> pathBuffer = MakeMutableStringBuffer(directoryPath);
    std::vector<wchar_t> resultBuffer(65536, L'\0');
    long rpcStatus = ERROR_GEN_FAILURE;

    const bool success = RpcCallScanDirectory(bindingHandle,
        pathBuffer.data(),
        static_cast<long>(resultBuffer.size()),
        resultBuffer.data(),
        &rpcStatus);

    RpcBindingFree(&bindingHandle);
    statusCode = static_cast<DWORD>(rpcStatus);
    resultText = resultBuffer.data();
    return success;
}

bool RequestFixedDrivesScan(DWORD& statusCode, std::wstring& resultText)
{
    statusCode = ERROR_GEN_FAILURE;
    resultText.clear();

    RPC_BINDING_HANDLE bindingHandle = nullptr;
    if (!CreateServiceBinding(bindingHandle))
    {
        return false;
    }

    std::vector<wchar_t> resultBuffer(65536, L'\0');
    long rpcStatus = ERROR_GEN_FAILURE;

    const bool success = RpcCallScanFixedDrives(bindingHandle,
        static_cast<long>(resultBuffer.size()),
        resultBuffer.data(),
        &rpcStatus);

    RpcBindingFree(&bindingHandle);
    statusCode = static_cast<DWORD>(rpcStatus);
    resultText = resultBuffer.data();
    return success;
}

bool ConfigureScheduledScan(bool enabled, const std::wstring& path, long intervalMinutes, DWORD& statusCode, std::wstring& resultText)
{
    statusCode = ERROR_GEN_FAILURE;
    resultText.clear();

    RPC_BINDING_HANDLE bindingHandle = nullptr;
    if (!CreateServiceBinding(bindingHandle))
    {
        return false;
    }

    std::vector<wchar_t> pathBuffer = MakeMutableStringBuffer(path);
    std::vector<wchar_t> resultBuffer(4096, L'\0');
    long rpcStatus = ERROR_GEN_FAILURE;

    const bool success = RpcCallSetScheduledScan(bindingHandle,
        enabled ? 1L : 0L,
        pathBuffer.data(),
        intervalMinutes,
        static_cast<long>(resultBuffer.size()),
        resultBuffer.data(),
        &rpcStatus);

    RpcBindingFree(&bindingHandle);
    statusCode = static_cast<DWORD>(rpcStatus);
    resultText = resultBuffer.data();
    return success;
}

bool GetScheduledScanResult(DWORD& statusCode, std::wstring& resultText)
{
    statusCode = ERROR_GEN_FAILURE;
    resultText.clear();

    RPC_BINDING_HANDLE bindingHandle = nullptr;
    if (!CreateServiceBinding(bindingHandle))
    {
        return false;
    }

    std::vector<wchar_t> resultBuffer(65536, L'\0');
    long rpcStatus = ERROR_GEN_FAILURE;

    const bool success = RpcCallGetScheduledScanResult(bindingHandle,
        static_cast<long>(resultBuffer.size()),
        resultBuffer.data(),
        &rpcStatus);

    RpcBindingFree(&bindingHandle);
    statusCode = static_cast<DWORD>(rpcStatus);
    resultText = resultBuffer.data();
    return success;
}

bool ConfigureMonitoredDirectory(bool enabled, const std::wstring& directoryPath, DWORD& statusCode, std::wstring& resultText)
{
    statusCode = ERROR_GEN_FAILURE;
    resultText.clear();

    RPC_BINDING_HANDLE bindingHandle = nullptr;
    if (!CreateServiceBinding(bindingHandle))
    {
        return false;
    }

    std::vector<wchar_t> pathBuffer = MakeMutableStringBuffer(directoryPath);
    std::vector<wchar_t> resultBuffer(4096, L'\0');
    long rpcStatus = ERROR_GEN_FAILURE;

    const bool success = RpcCallSetMonitoredDirectory(bindingHandle,
        enabled ? 1L : 0L,
        pathBuffer.data(),
        static_cast<long>(resultBuffer.size()),
        resultBuffer.data(),
        &rpcStatus);

    RpcBindingFree(&bindingHandle);
    statusCode = static_cast<DWORD>(rpcStatus);
    resultText = resultBuffer.data();
    return success;
}

bool GetMonitorResult(DWORD& statusCode, std::wstring& resultText)
{
    statusCode = ERROR_GEN_FAILURE;
    resultText.clear();

    RPC_BINDING_HANDLE bindingHandle = nullptr;
    if (!CreateServiceBinding(bindingHandle))
    {
        return false;
    }

    std::vector<wchar_t> resultBuffer(65536, L'\0');
    long rpcStatus = ERROR_GEN_FAILURE;

    const bool success = RpcCallGetMonitorResult(bindingHandle,
        static_cast<long>(resultBuffer.size()),
        resultBuffer.data(),
        &rpcStatus);

    RpcBindingFree(&bindingHandle);
    statusCode = static_cast<DWORD>(rpcStatus);
    resultText = resultBuffer.data();
    return success;
}

void SaveApplicationPathForService()
{
    wchar_t modulePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) == 0)
    {
        return;
    }

    const std::wstring path = ExpandPath(kApplicationPathFile);
    if (path.empty())
    {
        return;
    }

    std::wofstream file(path, std::ios::trunc);
    if (!file.is_open())
    {
        return;
    }

    file << modulePath;
}