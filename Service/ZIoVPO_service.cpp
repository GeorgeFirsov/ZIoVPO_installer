#define NOMINMAX

#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 0x00000800
#endif

#include "ZIoVPO_service.h"
#include "ServiceControlRpc_h.h"
#include "AvEngine.h"
#include "AvDatabaseStorage.h"

#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <iphlpapi.h>
#include <winhttp.h>
#include <rpc.h>
#include <aclapi.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#ifndef ERROR_BAD_SIGNATURE
#define ERROR_BAD_SIGNATURE 1918L
#endif

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Rpcrt4.lib")
#pragma comment(lib, "Userenv.lib")
#pragma comment(lib, "Wtsapi32.lib")
#pragma comment(lib, "Winhttp.lib")
#pragma comment(lib, "Iphlpapi.lib")

SERVICE_STATUS g_ServiceStatus = {};
SERVICE_STATUS_HANDLE g_StatusHandle = nullptr;
HANDLE g_StopEvent = nullptr;

namespace
{
    constexpr wchar_t kServiceName[] = L"ZIoVPO_service";
    constexpr wchar_t kRpcProtocolSequence[] = L"ncalrpc";
    constexpr wchar_t kRpcEndpoint[] = L"ZIoVPO_ServiceControl";
    constexpr wchar_t kApplicationPathFile[] = L"%PUBLIC%\\Documents\\ZIoVPO_ApplicationPath.txt";
    constexpr wchar_t kServerBaseUrlEnvironment[] = L"ZIOVPO_SERVER_BASE_URL";
    constexpr wchar_t kProductIdEnvironment[] = L"ZIOVPO_PRODUCT_ID";
    constexpr wchar_t kDefaultServerBaseUrl[] = L"https://localhost:8080";

    constexpr DWORD kHttpOk = 200;
    constexpr DWORD kHttpUnauthorized = 401;
    constexpr DWORD kHttpForbidden = 403;
    constexpr DWORD kHttpNotFound = 404;
    constexpr DWORD kHttpConflict = 409;

    constexpr long kRpcSuccess = ERROR_SUCCESS;
    constexpr long kRpcUnauthorized = ERROR_ACCESS_DENIED;
    constexpr long kRpcNotFound = ERROR_NOT_FOUND;
    constexpr long kRpcGenericFailure = ERROR_GEN_FAILURE;
    constexpr long kRpcInvalidData = ERROR_INVALID_DATA;
    constexpr long kRpcInvalidParameter = ERROR_INVALID_PARAMETER;
    constexpr long kRpcLogonFailure = ERROR_LOGON_FAILURE;

    struct TrackedProcess
    {
        DWORD sessionId = 0;
        DWORD processId = 0;
        HANDLE processHandle = nullptr;
    };

    struct AuthState
    {
        bool authenticated = false;
        std::wstring username;
        std::wstring accessToken;
        std::wstring refreshToken;
        std::time_t accessExpiry = 0;
        std::time_t refreshExpiry = 0;
        std::time_t nextRefreshAt = 0;
    };

    struct LicenseState
    {
        bool hasTicket = false;
        bool blocked = true;
        std::wstring endingDate;
        long long ttlSeconds = 0;
        std::time_t nextRefreshAt = 0;
    };

    struct UrlParts
    {
        bool secure = true;
        INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
        std::wstring host;
        std::wstring basePath;
    };

    struct HttpResponse
    {
        bool transportOk = false;
        DWORD transportError = ERROR_SUCCESS;
        DWORD statusCode = 0;
        std::wstring body;
    };

    struct HttpBinaryResponse
    {
        bool transportOk = false;
        DWORD transportError = ERROR_SUCCESS;
        DWORD statusCode = 0;
        std::string contentType;
        std::vector<unsigned char> body;
    };

    struct ScheduledScanState
    {
        bool enabled = false;
        std::wstring path;
        DWORD intervalMinutes = 0;
        std::time_t nextRunAt = 0;
        std::wstring lastResult;
    };

    struct MonitorState
    {
        bool enabled = false;
        std::wstring directory;
        bool snapshotInitialized = false;
        std::map<std::wstring, long long> snapshot;
        std::wstring lastResult;
    };

    std::mutex g_TrackedProcessesMutex;
    std::vector<TrackedProcess> g_TrackedProcesses;

    std::mutex g_StateMutex;
    AuthState g_AuthState;
    LicenseState g_LicenseState;

    std::mutex g_AvDatabaseMutex;
    AvEngine::AvDatabase g_AvDatabase;

    std::mutex g_ScheduledScanMutex;
    ScheduledScanState g_ScheduledScanState;

    std::mutex g_MonitorMutex;
    MonitorState g_MonitorState;

    HANDLE g_AuthWorkerEvent = nullptr;
    HANDLE g_LicenseWorkerEvent = nullptr;
    HANDLE g_ScheduledScanWorkerEvent = nullptr;
    HANDLE g_MonitorWorkerEvent = nullptr;
    HANDLE g_AvUpdateWorkerEvent = nullptr;
    HANDLE g_AuthWorkerThread = nullptr;
    HANDLE g_LicenseWorkerThread = nullptr;
    HANDLE g_ScheduledScanWorkerThread = nullptr;
    HANDLE g_MonitorWorkerThread = nullptr;
    HANDLE g_AvUpdateWorkerThread = nullptr;

    volatile LONG g_StopRequested = 0;

    void ClearAvDatabase();
    bool IsLicenseUnlocked();
    long LoadAvDatabaseNow();
    long UpdateAvDatabaseNow(bool forceFullUpdate);
    bool DatabasePackageExistsInDirectory(const std::wstring& directory);
    bool AnyProgramDataDatabasePackageExists();
    long CopyDatabaseForScanning(AvEngine::AvDatabase& database);
    std::wstring RunScanForPath(const std::wstring& path);

    bool ProtectCurrentServiceProcessFromUserTermination()
    {
        SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
        PSID usersSid = nullptr;

        if (!AllocateAndInitializeSid(
            &ntAuthority,
            2,
            SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_USERS,
            0, 0, 0, 0, 0, 0,
            &usersSid))
        {
            return false;
        }

        PSECURITY_DESCRIPTOR securityDescriptor = nullptr;
        PACL oldDacl = nullptr;
        PACL newDacl = nullptr;

        DWORD status = GetSecurityInfo(
            GetCurrentProcess(),
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
            BuildTrusteeWithSidW(&denyEntry.Trustee, usersSid);

            status = SetEntriesInAclW(1, &denyEntry, oldDacl, &newDacl);
            if (status == ERROR_SUCCESS)
            {
                status = SetSecurityInfo(
                    GetCurrentProcess(),
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
        if (usersSid != nullptr)
        {
            FreeSid(usersSid);
        }

        return status == ERROR_SUCCESS;
    }

    std::time_t GetCurrentTimeUtc()
    {
        return std::time(nullptr);
    }

    std::wstring TrimCopy(const std::wstring& value)
    {
        size_t start = 0;
        while (start < value.size() && iswspace(value[start]))
        {
            ++start;
        }

        size_t end = value.size();
        while (end > start && iswspace(value[end - 1]))
        {
            --end;
        }

        return value.substr(start, end - start);
    }

    void SetOutputString(wchar_t* buffer, long bufferLength, const std::wstring& value)
    {
        if (buffer == nullptr || bufferLength <= 0)
        {
            return;
        }

        const size_t maxCopy = static_cast<size_t>(bufferLength - 1);
        const size_t copyCount = value.size() < maxCopy ? value.size() : maxCopy;
        if (copyCount > 0)
        {
            wmemcpy(buffer, value.c_str(), copyCount);
        }
        buffer[copyCount] = L'\0';
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

    std::wstring GetEnvironmentValue(const wchar_t* name)
    {
        DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
        if (required == 0)
        {
            return L"";
        }

        std::wstring value(required, L'\0');
        GetEnvironmentVariableW(name, value.data(), required);

        while (!value.empty() && value.back() == L'\0')
        {
            value.pop_back();
        }

        return TrimCopy(value);
    }

    std::wstring GetServerBaseUrl()
    {
        const std::wstring value = GetEnvironmentValue(kServerBaseUrlEnvironment);
        return value.empty() ? std::wstring(kDefaultServerBaseUrl) : value;
    }

    std::wstring GetProductId()
    {
        return GetEnvironmentValue(kProductIdEnvironment);
    }

    std::wstring GetApplicationPath()
    {
        const std::wstring path = ExpandPath(kApplicationPathFile);
        if (path.empty())
        {
            return L"";
        }

        std::wifstream file(path);
        if (!file.is_open())
        {
            return L"";
        }

        std::wstring applicationPath;
        std::getline(file, applicationPath);
        return TrimCopy(applicationPath);
    }

    std::wstring GetDirectoryFromPath(const std::wstring& path)
    {
        const size_t slashPos = path.find_last_of(L"\\/");
        if (slashPos == std::wstring::npos)
        {
            return L"";
        }

        return path.substr(0, slashPos);
    }

    bool IsProcessRunning(HANDLE processHandle)
    {
        if (processHandle == nullptr)
        {
            return false;
        }

        DWORD waitResult = WaitForSingleObject(processHandle, 0);
        return waitResult == WAIT_TIMEOUT;
    }

    void CleanupTrackedProcesses()
    {
        std::lock_guard<std::mutex> lock(g_TrackedProcessesMutex);

        std::vector<TrackedProcess> aliveProcesses;
        aliveProcesses.reserve(g_TrackedProcesses.size());

        for (TrackedProcess& trackedProcess : g_TrackedProcesses)
        {
            if (IsProcessRunning(trackedProcess.processHandle))
            {
                aliveProcesses.push_back(trackedProcess);
            }
            else if (trackedProcess.processHandle != nullptr)
            {
                CloseHandle(trackedProcess.processHandle);
            }
        }

        g_TrackedProcesses.swap(aliveProcesses);
    }

    bool IsSessionAlreadyLaunched(DWORD sessionId)
    {
        std::lock_guard<std::mutex> lock(g_TrackedProcessesMutex);

        for (const TrackedProcess& trackedProcess : g_TrackedProcesses)
        {
            if (trackedProcess.sessionId == sessionId && IsProcessRunning(trackedProcess.processHandle))
            {
                return true;
            }
        }

        return false;
    }

    void TrackProcess(DWORD sessionId, HANDLE processHandle, DWORD processId)
    {
        std::lock_guard<std::mutex> lock(g_TrackedProcessesMutex);
        g_TrackedProcesses.push_back({ sessionId, processId, processHandle });
    }

    void TerminateAllLaunchedApplications()
    {
        std::vector<TrackedProcess> processesToStop;

        {
            std::lock_guard<std::mutex> lock(g_TrackedProcessesMutex);
            processesToStop.swap(g_TrackedProcesses);
        }

        for (TrackedProcess& trackedProcess : processesToStop)
        {
            if (trackedProcess.processHandle == nullptr)
            {
                continue;
            }

            if (IsProcessRunning(trackedProcess.processHandle))
            {
                TerminateProcess(trackedProcess.processHandle, 0);
                WaitForSingleObject(trackedProcess.processHandle, 5000);
            }

            CloseHandle(trackedProcess.processHandle);
            trackedProcess.processHandle = nullptr;
        }
    }

    bool LaunchInSession(DWORD sessionId)
    {
        if (sessionId == 0)
        {
            return false;
        }

        CleanupTrackedProcesses();

        if (IsSessionAlreadyLaunched(sessionId))
        {
            return true;
        }

        const std::wstring applicationPath = GetApplicationPath();
        if (applicationPath.empty())
        {
            return false;
        }

        HANDLE userToken = nullptr;
        if (!WTSQueryUserToken(sessionId, &userToken))
        {
            return false;
        }

        HANDLE primaryToken = nullptr;
        if (!DuplicateTokenEx(
            userToken,
            TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID,
            nullptr,
            SecurityImpersonation,
            TokenPrimary,
            &primaryToken))
        {
            CloseHandle(userToken);
            return false;
        }

        LPVOID environment = nullptr;
        CreateEnvironmentBlock(&environment, primaryToken, FALSE);

        std::wstring commandLine = L"\"" + applicationPath + L"\" --service-child --hidden";
        std::vector<wchar_t> commandBuffer(commandLine.begin(), commandLine.end());
        commandBuffer.push_back(L'\0');

        STARTUPINFOW startupInfo = {};
        startupInfo.cb = sizeof(startupInfo);
        startupInfo.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
        startupInfo.dwFlags = STARTF_USESHOWWINDOW;
        startupInfo.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION processInformation = {};
        const std::wstring currentDirectory = GetDirectoryFromPath(applicationPath);

        const bool success = CreateProcessAsUserW(
            primaryToken,
            applicationPath.c_str(),
            commandBuffer.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_UNICODE_ENVIRONMENT,
            environment,
            currentDirectory.empty() ? nullptr : currentDirectory.c_str(),
            &startupInfo,
            &processInformation) != FALSE;

        if (environment != nullptr)
        {
            DestroyEnvironmentBlock(environment);
        }

        CloseHandle(primaryToken);
        CloseHandle(userToken);

        if (!success)
        {
            return false;
        }

        CloseHandle(processInformation.hThread);
        TrackProcess(sessionId, processInformation.hProcess, processInformation.dwProcessId);
        return true;
    }

    void LaunchForAllSessions()
    {
        PWTS_SESSION_INFO sessionInfo = nullptr;
        DWORD sessionCount = 0;

        if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessionInfo, &sessionCount))
        {
            return;
        }

        for (DWORD index = 0; index < sessionCount; ++index)
        {
            LaunchInSession(sessionInfo[index].SessionId);
        }

        WTSFreeMemory(sessionInfo);
    }

    std::wstring Utf8ToWide(const std::string& utf8)
    {
        if (utf8.empty())
        {
            return L"";
        }

        const int required = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
        if (required <= 0)
        {
            return L"";
        }

        std::wstring wide(required, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), wide.data(), required);
        return wide;
    }

    std::string WideToUtf8(const std::wstring& wide)
    {
        if (wide.empty())
        {
            return std::string();
        }

        const int required = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        if (required <= 0)
        {
            return std::string();
        }

        std::string utf8(required, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), utf8.data(), required, nullptr, nullptr);
        return utf8;
    }

    std::wstring JsonEscape(const std::wstring& value)
    {
        std::wstring result;
        result.reserve(value.size() + 16);

        for (wchar_t ch : value)
        {
            switch (ch)
            {
            case L'\\': result += L"\\\\"; break;
            case L'\"': result += L"\\\""; break;
            case L'\n': result += L"\\n"; break;
            case L'\r': result += L"\\r"; break;
            case L'\t': result += L"\\t"; break;
            default: result += ch; break;
            }
        }

        return result;
    }

    std::wstring BuildTransportErrorMessage(const wchar_t* operation, DWORD errorCode)
    {
        std::wstring message = operation;
        message += L": transport error ";
        message += std::to_wstring(errorCode);

        wchar_t* systemMessage = nullptr;
        const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
        const DWORD length = FormatMessageW(flags,
            nullptr,
            errorCode,
            0,
            reinterpret_cast<LPWSTR>(&systemMessage),
            0,
            nullptr);

        if (length > 0 && systemMessage != nullptr)
        {
            message += L" (";
            message += TrimCopy(systemMessage);
            message += L")";
        }

        if (systemMessage != nullptr)
        {
            LocalFree(systemMessage);
        }

        return message;
    }

    bool ParseBaseUrl(const std::wstring& url, UrlParts& parts)
    {
        std::wstring working = TrimCopy(url);
        parts = {};

        if (working.rfind(L"https://", 0) == 0)
        {
            parts.secure = true;
            working.erase(0, 8);
            parts.port = INTERNET_DEFAULT_HTTPS_PORT;
        }
        else if (working.rfind(L"http://", 0) == 0)
        {
            parts.secure = false;
            working.erase(0, 7);
            parts.port = INTERNET_DEFAULT_HTTP_PORT;
        }
        else
        {
            return false;
        }

        size_t slashPos = working.find(L'/');
        std::wstring hostPort = slashPos == std::wstring::npos ? working : working.substr(0, slashPos);
        parts.basePath = slashPos == std::wstring::npos ? L"" : working.substr(slashPos);

        size_t colonPos = hostPort.find(L':');
        if (colonPos == std::wstring::npos)
        {
            parts.host = hostPort;
        }
        else
        {
            parts.host = hostPort.substr(0, colonPos);
            try
            {
                parts.port = static_cast<INTERNET_PORT>(std::stoi(hostPort.substr(colonPos + 1)));
            }
            catch (...)
            {
                return false;
            }
        }

        if (parts.host.empty())
        {
            return false;
        }

        return true;
    }

    std::wstring BuildRequestPath(const UrlParts& urlParts, const std::wstring& path)
    {
        std::wstring fullPath = urlParts.basePath;
        if (!path.empty())
        {
            if (fullPath.empty())
            {
                fullPath = path;
            }
            else
            {
                const bool baseEndsWithSlash = fullPath.back() == L'/';
                const bool pathStartsWithSlash = path.front() == L'/';
                if (baseEndsWithSlash && pathStartsWithSlash)
                {
                    fullPath.pop_back();
                    fullPath += path;
                }
                else if (!baseEndsWithSlash && !pathStartsWithSlash)
                {
                    fullPath += L'/';
                    fullPath += path;
                }
                else
                {
                    fullPath += path;
                }
            }
        }
        return fullPath.empty() ? std::wstring(L"/") : fullPath;
    }

    HttpResponse PostJson(const std::wstring& path, const std::wstring& jsonBody, const std::wstring& bearerToken)
    {
        HttpResponse response;
        UrlParts urlParts;
        if (!ParseBaseUrl(GetServerBaseUrl(), urlParts))
        {
            response.transportError = ERROR_INVALID_PARAMETER;
            return response;
        }

        HINTERNET sessionHandle = WinHttpOpen(L"ZIoVPOService/1.0",
            WINHTTP_ACCESS_TYPE_NO_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);
        if (sessionHandle == nullptr)
        {
            response.transportError = GetLastError();
            return response;
        }

        DWORD secureProtocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(sessionHandle, WINHTTP_OPTION_SECURE_PROTOCOLS, &secureProtocols, sizeof(secureProtocols));

        HINTERNET connectHandle = WinHttpConnect(sessionHandle, urlParts.host.c_str(), urlParts.port, 0);
        if (connectHandle == nullptr)
        {
            response.transportError = GetLastError();
            WinHttpCloseHandle(sessionHandle);
            return response;
        }

        std::wstring fullPath = urlParts.basePath;
        if (fullPath.empty())
        {
            fullPath = L"";
        }

        if (!path.empty())
        {
            if (fullPath.empty())
            {
                fullPath = path;
            }
            else
            {
                const bool baseEndsWithSlash = fullPath.back() == L'/';
                const bool pathStartsWithSlash = path.front() == L'/';

                if (baseEndsWithSlash && pathStartsWithSlash)
                {
                    fullPath.pop_back();
                    fullPath += path;
                }
                else if (!baseEndsWithSlash && !pathStartsWithSlash)
                {
                    fullPath += L'/';
                    fullPath += path;
                }
                else
                {
                    fullPath += path;
                }
            }
        }

        if (fullPath.empty())
        {
            fullPath = L"/";
        }

        const DWORD requestFlags = urlParts.secure ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET requestHandle = WinHttpOpenRequest(connectHandle,
            L"POST",
            fullPath.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            requestFlags);
        if (requestHandle == nullptr)
        {
            response.transportError = GetLastError();
            WinHttpCloseHandle(connectHandle);
            WinHttpCloseHandle(sessionHandle);
            return response;
        }

        WinHttpSetTimeouts(requestHandle, 5000, 5000, 10000, 10000);

        if (urlParts.secure)
        {
            DWORD securityFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
            WinHttpSetOption(requestHandle, WINHTTP_OPTION_SECURITY_FLAGS, &securityFlags, sizeof(securityFlags));
        }

        std::wstring headers = L"Content-Type: application/json\r\nAccept: application/json\r\n";
        if (!bearerToken.empty())
        {
            headers += L"Authorization: Bearer " + bearerToken + L"\r\n";
        }

        const std::string bodyUtf8 = WideToUtf8(jsonBody);
        const DWORD bodyLength = static_cast<DWORD>(bodyUtf8.size());

        const BOOL sendOk = WinHttpSendRequest(
            requestHandle,
            headers.c_str(),
            static_cast<DWORD>(headers.length()),
            WINHTTP_NO_REQUEST_DATA,
            0,
            bodyLength,
            0);

        if (!sendOk)
        {
            response.transportError = GetLastError();
            WinHttpCloseHandle(requestHandle);
            WinHttpCloseHandle(connectHandle);
            WinHttpCloseHandle(sessionHandle);
            return response;
        }

        if (bodyLength > 0)
        {
            DWORD written = 0;
            if (!WinHttpWriteData(requestHandle, bodyUtf8.data(), bodyLength, &written) || written != bodyLength)
            {
                response.transportError = GetLastError();
                WinHttpCloseHandle(requestHandle);
                WinHttpCloseHandle(connectHandle);
                WinHttpCloseHandle(sessionHandle);
                return response;
            }
        }

        if (!WinHttpReceiveResponse(requestHandle, nullptr))
        {
            response.transportError = GetLastError();
            WinHttpCloseHandle(requestHandle);
            WinHttpCloseHandle(connectHandle);
            WinHttpCloseHandle(sessionHandle);
            return response;
        }

        DWORD statusCode = 0;
        DWORD size = sizeof(statusCode);
        if (!WinHttpQueryHeaders(requestHandle,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &statusCode,
            &size,
            WINHTTP_NO_HEADER_INDEX))
        {
            response.transportError = GetLastError();
            WinHttpCloseHandle(requestHandle);
            WinHttpCloseHandle(connectHandle);
            WinHttpCloseHandle(sessionHandle);
            return response;
        }

        response.statusCode = statusCode;
        response.transportOk = true;
        response.transportError = ERROR_SUCCESS;

        std::string responseBytes;
        while (true)
        {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(requestHandle, &available))
            {
                response.transportError = GetLastError();
                response.transportOk = false;
                break;
            }

            if (available == 0)
            {
                break;
            }

            std::string chunk(available, '\0');
            DWORD downloaded = 0;
            if (!WinHttpReadData(requestHandle, chunk.data(), available, &downloaded))
            {
                response.transportError = GetLastError();
                response.transportOk = false;
                break;
            }

            if (downloaded == 0)
            {
                break;
            }

            chunk.resize(downloaded);
            responseBytes += chunk;
        }

        response.body = Utf8ToWide(responseBytes);

        WinHttpCloseHandle(requestHandle);
        WinHttpCloseHandle(connectHandle);
        WinHttpCloseHandle(sessionHandle);
        return response;
    }

    HttpResponse GetJson(const std::wstring& path, const std::wstring& bearerToken)
    {
        HttpResponse response;
        UrlParts urlParts;
        if (!ParseBaseUrl(GetServerBaseUrl(), urlParts))
        {
            response.transportError = ERROR_INVALID_PARAMETER;
            return response;
        }

        HINTERNET sessionHandle = WinHttpOpen(L"ZIoVPOService/1.0",
            WINHTTP_ACCESS_TYPE_NO_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);
        if (sessionHandle == nullptr)
        {
            response.transportError = GetLastError();
            return response;
        }

        DWORD secureProtocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(sessionHandle, WINHTTP_OPTION_SECURE_PROTOCOLS, &secureProtocols, sizeof(secureProtocols));

        HINTERNET connectHandle = WinHttpConnect(sessionHandle, urlParts.host.c_str(), urlParts.port, 0);
        if (connectHandle == nullptr)
        {
            response.transportError = GetLastError();
            WinHttpCloseHandle(sessionHandle);
            return response;
        }

        std::wstring fullPath = urlParts.basePath;
        if (!path.empty())
        {
            if (fullPath.empty())
            {
                fullPath = path;
            }
            else
            {
                const bool baseEndsWithSlash = fullPath.back() == L'/';
                const bool pathStartsWithSlash = path.front() == L'/';

                if (baseEndsWithSlash && pathStartsWithSlash)
                {
                    fullPath.pop_back();
                    fullPath += path;
                }
                else if (!baseEndsWithSlash && !pathStartsWithSlash)
                {
                    fullPath += L'/';
                    fullPath += path;
                }
                else
                {
                    fullPath += path;
                }
            }
        }

        if (fullPath.empty())
        {
            fullPath = L"/";
        }

        const DWORD requestFlags = urlParts.secure ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET requestHandle = WinHttpOpenRequest(connectHandle,
            L"GET",
            fullPath.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            requestFlags);
        if (requestHandle == nullptr)
        {
            response.transportError = GetLastError();
            WinHttpCloseHandle(connectHandle);
            WinHttpCloseHandle(sessionHandle);
            return response;
        }

        WinHttpSetTimeouts(requestHandle, 5000, 5000, 10000, 10000);

        if (urlParts.secure)
        {
            DWORD securityFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
            WinHttpSetOption(requestHandle, WINHTTP_OPTION_SECURITY_FLAGS, &securityFlags, sizeof(securityFlags));
        }

        std::wstring headers = L"Accept: application/json\r\n";
        if (!bearerToken.empty())
        {
            headers += L"Authorization: Bearer " + bearerToken + L"\r\n";
        }

        if (!WinHttpSendRequest(requestHandle,
            headers.c_str(),
            static_cast<DWORD>(headers.length()),
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0))
        {
            response.transportError = GetLastError();
            WinHttpCloseHandle(requestHandle);
            WinHttpCloseHandle(connectHandle);
            WinHttpCloseHandle(sessionHandle);
            return response;
        }

        if (!WinHttpReceiveResponse(requestHandle, nullptr))
        {
            response.transportError = GetLastError();
            WinHttpCloseHandle(requestHandle);
            WinHttpCloseHandle(connectHandle);
            WinHttpCloseHandle(sessionHandle);
            return response;
        }

        DWORD statusCode = 0;
        DWORD size = sizeof(statusCode);
        if (!WinHttpQueryHeaders(requestHandle,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &statusCode,
            &size,
            WINHTTP_NO_HEADER_INDEX))
        {
            response.transportError = GetLastError();
            WinHttpCloseHandle(requestHandle);
            WinHttpCloseHandle(connectHandle);
            WinHttpCloseHandle(sessionHandle);
            return response;
        }

        response.statusCode = statusCode;
        response.transportOk = true;
        response.transportError = ERROR_SUCCESS;

        std::string responseBytes;
        while (true)
        {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(requestHandle, &available))
            {
                response.transportError = GetLastError();
                response.transportOk = false;
                break;
            }

            if (available == 0)
            {
                break;
            }

            std::string chunk(available, '\0');
            DWORD downloaded = 0;
            if (!WinHttpReadData(requestHandle, chunk.data(), available, &downloaded))
            {
                response.transportError = GetLastError();
                response.transportOk = false;
                break;
            }

            if (downloaded == 0)
            {
                break;
            }

            chunk.resize(downloaded);
            responseBytes += chunk;
        }

        response.body = Utf8ToWide(responseBytes);

        WinHttpCloseHandle(requestHandle);
        WinHttpCloseHandle(connectHandle);
        WinHttpCloseHandle(sessionHandle);
        return response;
    }

    bool ExtractJsonRawValue(const std::wstring& json, const std::wstring& key, std::wstring& rawValue)
    {
        const std::wstring quotedKey = L"\"" + key + L"\"";
        size_t keyPos = json.find(quotedKey);
        if (keyPos == std::wstring::npos)
        {
            return false;
        }

        size_t colonPos = json.find(L':', keyPos + quotedKey.size());
        if (colonPos == std::wstring::npos)
        {
            return false;
        }

        size_t valuePos = colonPos + 1;
        while (valuePos < json.size() && iswspace(json[valuePos]))
        {
            ++valuePos;
        }

        if (valuePos >= json.size())
        {
            return false;
        }

        if (json[valuePos] == L'\"')
        {
            ++valuePos;
            std::wstring value;
            bool escaped = false;
            for (size_t index = valuePos; index < json.size(); ++index)
            {
                const wchar_t ch = json[index];
                if (escaped)
                {
                    switch (ch)
                    {
                    case L'\"': value += L'\"'; break;
                    case L'\\': value += L'\\'; break;
                    case L'/': value += L'/'; break;
                    case L'b': value += L'\b'; break;
                    case L'f': value += L'\f'; break;
                    case L'n': value += L'\n'; break;
                    case L'r': value += L'\r'; break;
                    case L't': value += L'\t'; break;
                    default: value += ch; break;
                    }
                    escaped = false;
                    continue;
                }

                if (ch == L'\\')
                {
                    escaped = true;
                    continue;
                }

                if (ch == L'\"')
                {
                    rawValue = value;
                    return true;
                }

                value += ch;
            }

            return false;
        }

        size_t endPos = valuePos;
        while (endPos < json.size() && json[endPos] != L',' && json[endPos] != L'}' && !iswspace(json[endPos]))
        {
            ++endPos;
        }

        rawValue = json.substr(valuePos, endPos - valuePos);
        return true;
    }

    bool ExtractJsonString(const std::wstring& json, const std::wstring& key, std::wstring& value)
    {
        return ExtractJsonRawValue(json, key, value);
    }

    bool ExtractJsonLongLong(const std::wstring& json, const std::wstring& key, long long& value)
    {
        std::wstring raw;
        if (!ExtractJsonRawValue(json, key, raw))
        {
            return false;
        }

        try
        {
            value = std::stoll(raw);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool ExtractJsonBool(const std::wstring& json, const std::wstring& key, bool& value)
    {
        std::wstring raw;
        if (!ExtractJsonRawValue(json, key, raw))
        {
            return false;
        }

        if (raw == L"true")
        {
            value = true;
            return true;
        }

        if (raw == L"false")
        {
            value = false;
            return true;
        }

        return false;
    }

    std::vector<unsigned char> Base64UrlDecode(const std::wstring& value)
    {
        std::string input;
        input.reserve(value.size());

        for (wchar_t ch : value)
        {
            if (ch == L'-')
            {
                input.push_back('+');
            }
            else if (ch == L'_')
            {
                input.push_back('/');
            }
            else if (ch <= 0x7F)
            {
                input.push_back(static_cast<char>(ch));
            }
        }

        while (input.size() % 4 != 0)
        {
            input.push_back('=');
        }

        auto decodeChar = [](char ch) -> int
            {
                if (ch >= 'A' && ch <= 'Z') return ch - 'A';
                if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
                if (ch >= '0' && ch <= '9') return ch - '0' + 52;
                if (ch == '+') return 62;
                if (ch == '/') return 63;
                return -1;
            };

        std::vector<unsigned char> output;
        output.reserve((input.size() / 4) * 3);

        for (size_t index = 0; index + 3 < input.size(); index += 4)
        {
            const int c1 = decodeChar(input[index]);
            const int c2 = decodeChar(input[index + 1]);
            const int c3 = input[index + 2] == '=' ? -1 : decodeChar(input[index + 2]);
            const int c4 = input[index + 3] == '=' ? -1 : decodeChar(input[index + 3]);

            if (c1 < 0 || c2 < 0)
            {
                return {};
            }

            output.push_back(static_cast<unsigned char>((c1 << 2) | (c2 >> 4)));

            if (c3 >= 0)
            {
                output.push_back(static_cast<unsigned char>(((c2 & 0x0F) << 4) | (c3 >> 2)));

                if (c4 >= 0)
                {
                    output.push_back(static_cast<unsigned char>(((c3 & 0x03) << 6) | c4));
                }
            }
        }

        return output;
    }

    bool ParseJwtExpiry(const std::wstring& token, std::time_t& expiry)
    {
        if (token.empty())
        {
            return false;
        }

        size_t firstDot = token.find(L'.');
        if (firstDot == std::wstring::npos)
        {
            return false;
        }

        size_t secondDot = token.find(L'.', firstDot + 1);
        if (secondDot == std::wstring::npos)
        {
            return false;
        }

        const std::wstring payloadPart = token.substr(firstDot + 1, secondDot - firstDot - 1);
        const std::vector<unsigned char> decoded = Base64UrlDecode(payloadPart);
        if (decoded.empty())
        {
            return false;
        }

        const std::string payloadUtf8(decoded.begin(), decoded.end());
        const std::wstring payload = Utf8ToWide(payloadUtf8);

        long long expValue = 0;
        if (!ExtractJsonLongLong(payload, L"exp", expValue))
        {
            return false;
        }

        expiry = static_cast<std::time_t>(expValue);
        return true;
    }

    std::time_t CalculateNextTokenRefreshAt(std::time_t accessExpiry)
    {
        const std::time_t now = GetCurrentTimeUtc();
        std::time_t nextTime = accessExpiry - 60;
        if (nextTime <= now)
        {
            nextTime = now + 5;
        }
        return nextTime;
    }

    std::time_t CalculateNextLicenseRefreshAt(long long ttlSeconds)
    {
        const std::time_t now = GetCurrentTimeUtc();
        long long refreshAfter = ttlSeconds - 30;
        if (refreshAfter < 5)
        {
            refreshAfter = 5;
        }
        return now + static_cast<std::time_t>(refreshAfter);
    }

    void ClearLicenseStateLocked()
    {
        g_LicenseState = {};
        g_LicenseState.blocked = true;
    }

    void ClearAuthStateLocked()
    {
        g_AuthState = {};
        ClearLicenseStateLocked();
    }

    void SignalAuthWorker()
    {
        if (g_AuthWorkerEvent != nullptr)
        {
            SetEvent(g_AuthWorkerEvent);
        }
    }

    void SignalLicenseWorker()
    {
        if (g_LicenseWorkerEvent != nullptr)
        {
            SetEvent(g_LicenseWorkerEvent);
        }
    }

    void SignalScheduledScanWorker()
    {
        if (g_ScheduledScanWorkerEvent != nullptr)
        {
            SetEvent(g_ScheduledScanWorkerEvent);
        }
    }

    void SignalMonitorWorker()
    {
        if (g_MonitorWorkerEvent != nullptr)
        {
            SetEvent(g_MonitorWorkerEvent);
        }
    }

    void SignalAvUpdateWorker()
    {
        if (g_AvUpdateWorkerEvent != nullptr)
        {
            SetEvent(g_AvUpdateWorkerEvent);
        }
    }

    void ClearAuthAndLicenseState()
    {
        {
            std::lock_guard<std::mutex> lock(g_StateMutex);
            ClearAuthStateLocked();
        }
        ClearAvDatabase();
        SignalAuthWorker();
        SignalLicenseWorker();
        SignalScheduledScanWorker();
        SignalMonitorWorker();
        SignalAvUpdateWorker();
    }

    std::wstring GetComputerNameString()
    {
        wchar_t buffer[MAX_COMPUTERNAME_LENGTH + 1] = {};
        DWORD size = ARRAYSIZE(buffer);
        if (GetComputerNameW(buffer, &size))
        {
            return buffer;
        }
        return L"WindowsDevice";
    }

    std::wstring GetPrimaryMacAddress()
    {
        ULONG bufferLength = 0;
        if (GetAdaptersInfo(nullptr, &bufferLength) != ERROR_BUFFER_OVERFLOW || bufferLength == 0)
        {
            return L"";
        }

        std::vector<unsigned char> buffer(bufferLength);
        PIP_ADAPTER_INFO adapterInfo = reinterpret_cast<PIP_ADAPTER_INFO>(buffer.data());
        if (GetAdaptersInfo(adapterInfo, &bufferLength) != NO_ERROR)
        {
            return L"";
        }

        for (PIP_ADAPTER_INFO current = adapterInfo; current != nullptr; current = current->Next)
        {
            if (current->AddressLength == 0)
            {
                continue;
            }

            wchar_t macBuffer[32] = {};
            size_t offset = 0;
            for (UINT index = 0; index < current->AddressLength && offset + 4 < ARRAYSIZE(macBuffer); ++index)
            {
                const int written = swprintf_s(macBuffer + offset,
                    ARRAYSIZE(macBuffer) - offset,
                    index == 0 ? L"%02X" : L"-%02X",
                    current->Address[index]);
                if (written <= 0)
                {
                    break;
                }
                offset += static_cast<size_t>(written);
            }

            if (macBuffer[0] != L'\0')
            {
                return macBuffer;
            }
        }

        return L"";
    }

    void SaveAuthTokens(const std::wstring& username, const std::wstring& accessToken, const std::wstring& refreshToken)
    {
        const std::time_t now = GetCurrentTimeUtc();

        std::time_t accessExpiry = 0;
        if (!ParseJwtExpiry(accessToken, accessExpiry))
        {
            accessExpiry = now + 15 * 60;
        }

        std::time_t refreshExpiry = 0;
        if (!ParseJwtExpiry(refreshToken, refreshExpiry))
        {
            refreshExpiry = now + 24 * 60 * 60;
        }

        std::lock_guard<std::mutex> lock(g_StateMutex);
        g_AuthState.authenticated = true;
        g_AuthState.username = username;
        g_AuthState.accessToken = accessToken;
        g_AuthState.refreshToken = refreshToken;
        g_AuthState.accessExpiry = accessExpiry;
        g_AuthState.refreshExpiry = refreshExpiry;
        g_AuthState.nextRefreshAt = CalculateNextTokenRefreshAt(accessExpiry);
    }

    bool SaveLicenseTicketFromResponse(const std::wstring& responseBody)
    {
        std::wstring endingDate;
        long long ttlSeconds = 0;
        bool blocked = false;

        if (!ExtractJsonString(responseBody, L"endingDate", endingDate))
        {
            return false;
        }
        if (!ExtractJsonLongLong(responseBody, L"ttlSeconds", ttlSeconds))
        {
            return false;
        }
        if (!ExtractJsonBool(responseBody, L"blocked", blocked))
        {
            return false;
        }

        std::lock_guard<std::mutex> lock(g_StateMutex);
        g_LicenseState.hasTicket = true;
        g_LicenseState.blocked = blocked;
        g_LicenseState.endingDate = endingDate;
        g_LicenseState.ttlSeconds = ttlSeconds;
        g_LicenseState.nextRefreshAt = CalculateNextLicenseRefreshAt(ttlSeconds);
        return true;
    }

    long RefreshTokensNow(bool clearStateOnFailure)
    {
        std::wstring refreshToken;
        std::wstring username;
        {
            std::lock_guard<std::mutex> lock(g_StateMutex);
            refreshToken = g_AuthState.refreshToken;
            username = g_AuthState.username;
        }

        if (refreshToken.empty())
        {
            if (clearStateOnFailure)
            {
                ClearAuthAndLicenseState();
            }
            return kRpcUnauthorized;
        }

        const std::wstring requestBody = L"{\"refreshToken\":\"" + JsonEscape(refreshToken) + L"\"}";
        const HttpResponse response = PostJson(L"/api/auth/refresh", requestBody, L"");
        if (!response.transportOk)
        {
            if (clearStateOnFailure)
            {
                ClearAuthAndLicenseState();
            }
            return response.transportError != ERROR_SUCCESS ? static_cast<long>(response.transportError) : kRpcGenericFailure;
        }

        if (response.statusCode != kHttpOk)
        {
            if (clearStateOnFailure)
            {
                ClearAuthAndLicenseState();
            }
            return kRpcUnauthorized;
        }

        std::wstring accessToken;
        std::wstring newRefreshToken;
        if (!ExtractJsonString(response.body, L"accessToken", accessToken) ||
            !ExtractJsonString(response.body, L"refreshToken", newRefreshToken))
        {
            if (clearStateOnFailure)
            {
                ClearAuthAndLicenseState();
            }
            return kRpcInvalidData;
        }

        SaveAuthTokens(username, accessToken, newRefreshToken);
        SignalAuthWorker();
        return kRpcSuccess;
    }

    long AuthorizedPostJson(const std::wstring& path, const std::wstring& requestBody, HttpResponse& response)
    {
        std::wstring accessToken;
        {
            std::lock_guard<std::mutex> lock(g_StateMutex);
            accessToken = g_AuthState.accessToken;
        }

        if (accessToken.empty())
        {
            return kRpcUnauthorized;
        }

        response = PostJson(path, requestBody, accessToken);
        if (!response.transportOk)
        {
            return response.transportError != ERROR_SUCCESS ? static_cast<long>(response.transportError) : kRpcGenericFailure;
        }

        if (response.statusCode == kHttpUnauthorized)
        {
            const long refreshStatus = RefreshTokensNow(true);
            if (refreshStatus != kRpcSuccess)
            {
                return refreshStatus;
            }

            {
                std::lock_guard<std::mutex> lock(g_StateMutex);
                accessToken = g_AuthState.accessToken;
            }

            response = PostJson(path, requestBody, accessToken);
            if (!response.transportOk)
            {
                return response.transportError != ERROR_SUCCESS ? static_cast<long>(response.transportError) : kRpcGenericFailure;
            }
        }

        return kRpcSuccess;
    }

    void ClearAvDatabase()
    {
        std::lock_guard<std::mutex> lock(g_AvDatabaseMutex);
        g_AvDatabase = {};
    }

    long AuthorizedGetJson(const std::wstring& path, HttpResponse& response)
    {
        std::wstring accessToken;
        {
            std::lock_guard<std::mutex> lock(g_StateMutex);
            accessToken = g_AuthState.accessToken;
        }

        if (accessToken.empty())
        {
            return kRpcUnauthorized;
        }

        response = GetJson(path, accessToken);
        if (!response.transportOk)
        {
            return response.transportError != ERROR_SUCCESS ? static_cast<long>(response.transportError) : kRpcGenericFailure;
        }

        if (response.statusCode == kHttpUnauthorized)
        {
            const long refreshStatus = RefreshTokensNow(true);
            if (refreshStatus != kRpcSuccess)
            {
                return refreshStatus;
            }

            {
                std::lock_guard<std::mutex> lock(g_StateMutex);
                accessToken = g_AuthState.accessToken;
            }

            response = GetJson(path, accessToken);
            if (!response.transportOk)
            {
                return response.transportError != ERROR_SUCCESS ? static_cast<long>(response.transportError) : kRpcGenericFailure;
            }
        }

        return kRpcSuccess;
    }

    HttpBinaryResponse RequestBinary(const std::wstring& method,
        const std::wstring& path,
        const std::vector<unsigned char>& requestBody,
        const std::wstring& contentType,
        const std::wstring& bearerToken)
    {
        HttpBinaryResponse response;
        UrlParts urlParts;
        if (!ParseBaseUrl(GetServerBaseUrl(), urlParts))
        {
            response.transportError = ERROR_INVALID_PARAMETER;
            return response;
        }

        HINTERNET sessionHandle = WinHttpOpen(L"ZIoVPOService/1.0",
            WINHTTP_ACCESS_TYPE_NO_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);
        if (sessionHandle == nullptr)
        {
            response.transportError = GetLastError();
            return response;
        }

        DWORD secureProtocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(sessionHandle, WINHTTP_OPTION_SECURE_PROTOCOLS, &secureProtocols, sizeof(secureProtocols));

        HINTERNET connectHandle = WinHttpConnect(sessionHandle, urlParts.host.c_str(), urlParts.port, 0);
        if (connectHandle == nullptr)
        {
            response.transportError = GetLastError();
            WinHttpCloseHandle(sessionHandle);
            return response;
        }

        const DWORD requestFlags = urlParts.secure ? WINHTTP_FLAG_SECURE : 0;
        const std::wstring fullPath = BuildRequestPath(urlParts, path);
        HINTERNET requestHandle = WinHttpOpenRequest(connectHandle,
            method.c_str(),
            fullPath.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            requestFlags);
        if (requestHandle == nullptr)
        {
            response.transportError = GetLastError();
            WinHttpCloseHandle(connectHandle);
            WinHttpCloseHandle(sessionHandle);
            return response;
        }

        WinHttpSetTimeouts(requestHandle, 5000, 5000, 15000, 15000);
        if (urlParts.secure)
        {
            DWORD securityFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
            WinHttpSetOption(requestHandle, WINHTTP_OPTION_SECURITY_FLAGS, &securityFlags, sizeof(securityFlags));
        }

        std::wstring headers = L"Accept: multipart/mixed\r\n";
        if (!contentType.empty())
        {
            headers += L"Content-Type: " + contentType + L"\r\n";
        }
        if (!bearerToken.empty())
        {
            headers += L"Authorization: Bearer " + bearerToken + L"\r\n";
        }

        const DWORD bodyLength = static_cast<DWORD>(requestBody.size());
        if (!WinHttpSendRequest(requestHandle,
            headers.c_str(),
            static_cast<DWORD>(headers.length()),
            WINHTTP_NO_REQUEST_DATA,
            0,
            bodyLength,
            0))
        {
            response.transportError = GetLastError();
            WinHttpCloseHandle(requestHandle);
            WinHttpCloseHandle(connectHandle);
            WinHttpCloseHandle(sessionHandle);
            return response;
        }

        if (bodyLength > 0)
        {
            DWORD written = 0;
            if (!WinHttpWriteData(requestHandle, requestBody.data(), bodyLength, &written) || written != bodyLength)
            {
                response.transportError = GetLastError();
                WinHttpCloseHandle(requestHandle);
                WinHttpCloseHandle(connectHandle);
                WinHttpCloseHandle(sessionHandle);
                return response;
            }
        }

        if (!WinHttpReceiveResponse(requestHandle, nullptr))
        {
            response.transportError = GetLastError();
            WinHttpCloseHandle(requestHandle);
            WinHttpCloseHandle(connectHandle);
            WinHttpCloseHandle(sessionHandle);
            return response;
        }

        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        if (!WinHttpQueryHeaders(requestHandle,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &statusCode,
            &statusSize,
            WINHTTP_NO_HEADER_INDEX))
        {
            response.transportError = GetLastError();
            WinHttpCloseHandle(requestHandle);
            WinHttpCloseHandle(connectHandle);
            WinHttpCloseHandle(sessionHandle);
            return response;
        }

        wchar_t contentTypeBuffer[512] = {};
        DWORD contentTypeSize = sizeof(contentTypeBuffer);
        if (WinHttpQueryHeaders(requestHandle,
            WINHTTP_QUERY_CONTENT_TYPE,
            WINHTTP_HEADER_NAME_BY_INDEX,
            contentTypeBuffer,
            &contentTypeSize,
            WINHTTP_NO_HEADER_INDEX))
        {
            response.contentType = WideToUtf8(contentTypeBuffer);
        }

        response.statusCode = statusCode;
        response.transportOk = true;
        response.transportError = ERROR_SUCCESS;

        while (true)
        {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(requestHandle, &available))
            {
                response.transportOk = false;
                response.transportError = GetLastError();
                break;
            }
            if (available == 0)
            {
                break;
            }

            const size_t oldSize = response.body.size();
            response.body.resize(oldSize + available);
            DWORD downloaded = 0;
            if (!WinHttpReadData(requestHandle, response.body.data() + oldSize, available, &downloaded))
            {
                response.transportOk = false;
                response.transportError = GetLastError();
                break;
            }
            response.body.resize(oldSize + downloaded);
            if (downloaded == 0)
            {
                break;
            }
        }

        WinHttpCloseHandle(requestHandle);
        WinHttpCloseHandle(connectHandle);
        WinHttpCloseHandle(sessionHandle);
        return response;
    }

    bool ExtractMultipartParts(const HttpBinaryResponse& response, AvDatabaseStorage::BinaryPackage& package)
    {
        package = {};
        if (response.body.empty())
        {
            return false;
        }

        std::string bodyText(reinterpret_cast<const char*>(response.body.data()), response.body.size());
        std::string boundary;
        const std::string boundaryKey = "boundary=";
        size_t boundaryPos = response.contentType.find(boundaryKey);
        if (boundaryPos != std::string::npos)
        {
            boundary = response.contentType.substr(boundaryPos + boundaryKey.size());
            if (!boundary.empty() && boundary.front() == '"')
            {
                boundary.erase(boundary.begin());
                const size_t quote = boundary.find('"');
                if (quote != std::string::npos)
                {
                    boundary = boundary.substr(0, quote);
                }
            }
            const size_t semicolon = boundary.find(';');
            if (semicolon != std::string::npos)
            {
                boundary = boundary.substr(0, semicolon);
            }
        }

        if (boundary.empty())
        {
            if (bodyText.rfind("--", 0) != 0)
            {
                return false;
            }
            const size_t lineEnd = bodyText.find("\r\n");
            if (lineEnd == std::string::npos || lineEnd <= 2)
            {
                return false;
            }
            boundary = bodyText.substr(2, lineEnd - 2);
        }

        const std::string delimiter = "--" + boundary;
        size_t searchPos = 0;
        int partIndex = 0;
        while (true)
        {
            size_t partStart = bodyText.find(delimiter, searchPos);
            if (partStart == std::string::npos)
            {
                break;
            }
            partStart += delimiter.size();
            if (partStart + 2 <= bodyText.size() && bodyText.compare(partStart, 2, "--") == 0)
            {
                break;
            }
            if (partStart + 2 <= bodyText.size() && bodyText.compare(partStart, 2, "\r\n") == 0)
            {
                partStart += 2;
            }

            const size_t headerEnd = bodyText.find("\r\n\r\n", partStart);
            if (headerEnd == std::string::npos)
            {
                break;
            }
            const std::string headers = bodyText.substr(partStart, headerEnd - partStart);
            const size_t contentStart = headerEnd + 4;
            size_t contentEnd = bodyText.find("\r\n" + delimiter, contentStart);
            if (contentEnd == std::string::npos)
            {
                contentEnd = bodyText.find(delimiter, contentStart);
            }
            if (contentEnd == std::string::npos || contentEnd < contentStart)
            {
                break;
            }

            std::vector<unsigned char> partBytes(
                response.body.begin() + static_cast<std::ptrdiff_t>(contentStart),
                response.body.begin() + static_cast<std::ptrdiff_t>(contentEnd));

            if (headers.find("manifest.bin") != std::string::npos || headers.find("name=\"manifest\"") != std::string::npos || partIndex == 0)
            {
                package.manifest = std::move(partBytes);
            }
            else if (headers.find("data.bin") != std::string::npos || headers.find("name=\"data\"") != std::string::npos || partIndex == 1)
            {
                package.data = std::move(partBytes);
            }

            ++partIndex;
            searchPos = contentEnd + 2;
        }

        return !package.manifest.empty() && !package.data.empty();
    }

    long AuthorizedGetBinary(const std::wstring& path, HttpBinaryResponse& response)
    {
        std::wstring accessToken;
        {
            std::lock_guard<std::mutex> lock(g_StateMutex);
            accessToken = g_AuthState.accessToken;
        }
        if (accessToken.empty())
        {
            return kRpcUnauthorized;
        }

        response = RequestBinary(L"GET", path, {}, L"", accessToken);
        if (!response.transportOk)
        {
            return response.transportError != ERROR_SUCCESS ? static_cast<long>(response.transportError) : kRpcGenericFailure;
        }
        if (response.statusCode == kHttpUnauthorized)
        {
            const long refreshStatus = RefreshTokensNow(true);
            if (refreshStatus != kRpcSuccess)
            {
                return refreshStatus;
            }
            {
                std::lock_guard<std::mutex> lock(g_StateMutex);
                accessToken = g_AuthState.accessToken;
            }
            response = RequestBinary(L"GET", path, {}, L"", accessToken);
            if (!response.transportOk)
            {
                return response.transportError != ERROR_SUCCESS ? static_cast<long>(response.transportError) : kRpcGenericFailure;
            }
        }
        return kRpcSuccess;
    }

    long AuthorizedPostBinary(const std::wstring& path, const std::wstring& requestBody, HttpBinaryResponse& response)
    {
        std::wstring accessToken;
        {
            std::lock_guard<std::mutex> lock(g_StateMutex);
            accessToken = g_AuthState.accessToken;
        }
        if (accessToken.empty())
        {
            return kRpcUnauthorized;
        }

        const std::string bodyUtf8 = WideToUtf8(requestBody);
        const std::vector<unsigned char> bodyBytes(bodyUtf8.begin(), bodyUtf8.end());
        response = RequestBinary(L"POST", path, bodyBytes, L"application/json", accessToken);
        if (!response.transportOk)
        {
            return response.transportError != ERROR_SUCCESS ? static_cast<long>(response.transportError) : kRpcGenericFailure;
        }
        if (response.statusCode == kHttpUnauthorized)
        {
            const long refreshStatus = RefreshTokensNow(true);
            if (refreshStatus != kRpcSuccess)
            {
                return refreshStatus;
            }
            {
                std::lock_guard<std::mutex> lock(g_StateMutex);
                accessToken = g_AuthState.accessToken;
            }
            response = RequestBinary(L"POST", path, bodyBytes, L"application/json", accessToken);
            if (!response.transportOk)
            {
                return response.transportError != ERROR_SUCCESS ? static_cast<long>(response.transportError) : kRpcGenericFailure;
            }
        }
        return kRpcSuccess;
    }

    std::vector<AvEngine::AvRecord> FlattenDatabaseRecords(const AvEngine::AvDatabase& database)
    {
        std::vector<AvEngine::AvRecord> records;
        records.reserve(database.recordCount);
        for (const auto& item : database.recordsByPrefix)
        {
            records.insert(records.end(), item.second.begin(), item.second.end());
        }
        return records;
    }

    bool MergeDatabases(const AvEngine::AvDatabase& baseDatabase, const AvEngine::AvDatabase& additionalDatabase, AvEngine::AvDatabase& mergedDatabase)
    {
        std::vector<AvEngine::AvRecord> records = FlattenDatabaseRecords(baseDatabase);
        const std::vector<AvEngine::AvRecord> additionalRecords = FlattenDatabaseRecords(additionalDatabase);
        records.insert(records.end(), additionalRecords.begin(), additionalRecords.end());

        std::wstring errorMessage;
        return AvEngine::BuildDatabaseFromRecords(records, baseDatabase.releaseDate, mergedDatabase, errorMessage);
    }

    std::wstring BuildIdsJson(const std::vector<std::wstring>& ids)
    {
        std::wstring json = L"{\"ids\":[";
        bool first = true;
        for (const std::wstring& id : ids)
        {
            if (id.empty())
            {
                continue;
            }
            if (!first)
            {
                json += L",";
            }
            json += L"\"" + JsonEscape(id) + L"\"";
            first = false;
        }
        json += L"]}";
        return json;
    }

    long TryRepairInvalidRecordsFromServer(AvDatabaseStorage::LoadResult& loadResult)
    {
        if (loadResult.invalidRecordIds.empty() || !IsLicenseUnlocked())
        {
            return kRpcSuccess;
        }

        HttpBinaryResponse response;
        const long status = AuthorizedPostBinary(L"/api/binary/signatures/by-ids", BuildIdsJson(loadResult.invalidRecordIds), response);
        if (status != kRpcSuccess || response.statusCode != kHttpOk)
        {
            return status == kRpcSuccess ? kRpcGenericFailure : status;
        }

        AvDatabaseStorage::BinaryPackage package;
        if (!ExtractMultipartParts(response, package))
        {
            return kRpcInvalidData;
        }

        AvDatabaseStorage::LoadResult fetched;
        if (!AvDatabaseStorage::LoadFromPackage(package, AvDatabaseStorage::GetPublicCertificatePath(), fetched))
        {
            return kRpcInvalidData;
        }

        AvEngine::AvDatabase merged;
        if (!MergeDatabases(loadResult.database, fetched.database, merged))
        {
            return kRpcInvalidData;
        }

        loadResult.database = merged;
        loadResult.invalidRecordIds.clear();
        return kRpcSuccess;
    }

    bool ApplyLoadedDatabase(AvDatabaseStorage::LoadResult& loadResult)
    {
        if (!loadResult.success)
        {
            return false;
        }

        TryRepairInvalidRecordsFromServer(loadResult);

        std::lock_guard<std::mutex> lock(g_AvDatabaseMutex);
        g_AvDatabase = loadResult.database;
        return true;
    }

    long LoadDatabaseFromDirectoryAndApply(const std::wstring& directory)
    {
        AvDatabaseStorage::LoadResult result;
        if (!AvDatabaseStorage::LoadFromDirectory(directory, AvDatabaseStorage::GetPublicCertificatePath(), result))
        {
            return result.manifestSignatureFailed ? ERROR_BAD_SIGNATURE : kRpcInvalidData;
        }
        return ApplyLoadedDatabase(result) ? kRpcSuccess : kRpcInvalidData;
    }

    long DownloadFullDatabasePackage(AvDatabaseStorage::BinaryPackage& package)
    {
        HttpBinaryResponse response;
        const long status = AuthorizedGetBinary(L"/api/binary/signatures/full", response);
        if (status != kRpcSuccess)
        {
            return status;
        }
        if (response.statusCode == kHttpForbidden || response.statusCode == kHttpUnauthorized)
        {
            return kRpcUnauthorized;
        }
        if (response.statusCode != kHttpOk)
        {
            return kRpcGenericFailure;
        }
        return ExtractMultipartParts(response, package) ? kRpcSuccess : kRpcInvalidData;
    }

    long LoadDefaultDatabaseAndApply()
    {
        AvDatabaseStorage::LoadResult defaultResult;
        if (!AvDatabaseStorage::LoadFromDirectory(
            AvDatabaseStorage::GetDefaultDatabaseDirectory(),
            AvDatabaseStorage::GetPublicCertificatePath(),
            defaultResult))
        {
            return defaultResult.manifestSignatureFailed ? ERROR_BAD_SIGNATURE : kRpcInvalidData;
        }

        return ApplyLoadedDatabase(defaultResult) ? kRpcSuccess : kRpcInvalidData;
    }

    long RestoreBackupDatabaseAndApply(const std::wstring& currentDirectory, const std::wstring& backupDirectory)
    {
        AvDatabaseStorage::LoadResult backupResult;
        if (!AvDatabaseStorage::LoadFromDirectory(backupDirectory, AvDatabaseStorage::GetPublicCertificatePath(), backupResult))
        {
            return backupResult.manifestSignatureFailed ? ERROR_BAD_SIGNATURE : kRpcInvalidData;
        }

        std::wstring copyError;
        AvDatabaseStorage::CopyDirectoryFiles(backupDirectory, currentDirectory, copyError);
        return ApplyLoadedDatabase(backupResult) ? kRpcSuccess : kRpcInvalidData;
    }

    long UpdateAvDatabaseNow(bool forceFullUpdate)
    {
        UNREFERENCED_PARAMETER(forceFullUpdate);

        if (!IsLicenseUnlocked())
        {
            return kRpcUnauthorized;
        }

        const std::wstring currentDirectory = AvDatabaseStorage::GetProgramDataDatabaseDirectory();
        const std::wstring backupDirectory = AvDatabaseStorage::GetProgramDataBackupDirectory();
        std::wstring storageError;

        const bool hadCurrentDatabase = DatabasePackageExistsInDirectory(currentDirectory);
        if (hadCurrentDatabase)
        {
            AvDatabaseStorage::LoadResult backupCandidate;
            if (AvDatabaseStorage::LoadFromDirectory(currentDirectory, AvDatabaseStorage::GetPublicCertificatePath(), backupCandidate))
            {
                AvDatabaseStorage::CopyDirectoryFiles(currentDirectory, backupDirectory, storageError);
            }
        }

        AvDatabaseStorage::BinaryPackage downloadedPackage;
        const long downloadStatus = DownloadFullDatabasePackage(downloadedPackage);
        if (downloadStatus != kRpcSuccess)
        {
            return downloadStatus;
        }

        if (!AvDatabaseStorage::SavePackageToDirectory(currentDirectory, downloadedPackage, storageError))
        {
            return kRpcGenericFailure;
        }

        AvDatabaseStorage::LoadResult loaded;
        if (AvDatabaseStorage::LoadFromDirectory(currentDirectory, AvDatabaseStorage::GetPublicCertificatePath(), loaded) && ApplyLoadedDatabase(loaded))
        {
            if (!DatabasePackageExistsInDirectory(backupDirectory))
            {
                AvDatabaseStorage::CopyDirectoryFiles(currentDirectory, backupDirectory, storageError);
            }
            return kRpcSuccess;
        }

        if (DatabasePackageExistsInDirectory(backupDirectory) &&
            AvDatabaseStorage::CopyDirectoryFiles(backupDirectory, currentDirectory, storageError))
        {
            const long restoreStatus = LoadDatabaseFromDirectoryAndApply(currentDirectory);
            if (restoreStatus == kRpcSuccess)
            {
                return restoreStatus;
            }
        }

        return LoadDefaultDatabaseAndApply();
    }

    bool DatabasePackageExistsInDirectory(const std::wstring& directory)
    {
        if (directory.empty())
        {
            return false;
        }

        std::error_code manifestError;
        std::error_code dataError;
        const std::filesystem::path directoryPath(directory);
        const std::filesystem::path manifestPath = directoryPath / L"manifest.bin";
        const std::filesystem::path dataPath = directoryPath / L"data.bin";

        return std::filesystem::is_regular_file(manifestPath, manifestError) &&
            std::filesystem::is_regular_file(dataPath, dataError);
    }

    bool AnyProgramDataDatabasePackageExists()
    {
        return DatabasePackageExistsInDirectory(AvDatabaseStorage::GetProgramDataDatabaseDirectory()) ||
            DatabasePackageExistsInDirectory(AvDatabaseStorage::GetProgramDataBackupDirectory());
    }

    long LoadAvDatabaseNow()
    {
        const std::wstring currentDirectory = AvDatabaseStorage::GetProgramDataDatabaseDirectory();
        const std::wstring backupDirectory = AvDatabaseStorage::GetProgramDataBackupDirectory();

        const bool currentExists = DatabasePackageExistsInDirectory(currentDirectory);
        const bool backupExists = DatabasePackageExistsInDirectory(backupDirectory);

        AvDatabaseStorage::LoadResult currentResult;
        if (currentExists &&
            AvDatabaseStorage::LoadFromDirectory(currentDirectory, AvDatabaseStorage::GetPublicCertificatePath(), currentResult))
        {
            ApplyLoadedDatabase(currentResult);
            return kRpcSuccess;
        }

        if (currentExists && currentResult.manifestSignatureFailed && IsLicenseUnlocked())
        {
            const long updateStatus = UpdateAvDatabaseNow(true);
            if (updateStatus == kRpcSuccess)
            {
                return kRpcSuccess;
            }
        }

        if (backupExists)
        {
            const long restoreStatus = RestoreBackupDatabaseAndApply(currentDirectory, backupDirectory);
            if (restoreStatus == kRpcSuccess)
            {
                return kRpcSuccess;
            }
        }

        if (!currentExists && !backupExists && IsLicenseUnlocked())
        {
            const long updateStatus = UpdateAvDatabaseNow(true);
            if (updateStatus == kRpcSuccess)
            {
                return kRpcSuccess;
            }
        }

        const long defaultStatus = LoadDefaultDatabaseAndApply();
        if (defaultStatus == kRpcSuccess)
        {
            return kRpcSuccess;
        }

        ClearAvDatabase();
        if (currentResult.manifestSignatureFailed)
        {
            return ERROR_BAD_SIGNATURE;
        }
        return defaultStatus;
    }

    bool IsLicenseUnlocked()
    {
        std::lock_guard<std::mutex> lock(g_StateMutex);
        return g_AuthState.authenticated && g_LicenseState.hasTicket && !g_LicenseState.blocked;
    }

    long CopyDatabaseForScanning(AvEngine::AvDatabase& database)
    {
        if (!IsLicenseUnlocked())
        {
            return kRpcUnauthorized;
        }

        {
            std::lock_guard<std::mutex> lock(g_AvDatabaseMutex);
            database = g_AvDatabase;
        }

        if (!database.loaded)
        {
            const long loadStatus = LoadAvDatabaseNow();
            if (loadStatus != kRpcSuccess)
            {
                return loadStatus;
            }

            std::lock_guard<std::mutex> lock(g_AvDatabaseMutex);
            database = g_AvDatabase;
        }

        return database.loaded ? kRpcSuccess : kRpcInvalidData;
    }

    std::wstring RunScanForPath(const std::wstring& path)
    {
        AvEngine::AvDatabase database;
        const long status = CopyDatabaseForScanning(database);
        if (status != kRpcSuccess)
        {
            return L"Scan is not available. Error code: " + std::to_wstring(status) + L"\r\n";
        }

        return AvEngine::ScanPath(database, path);
    }

    long CheckLicenseNow()
    {
        const std::wstring productId = GetProductId();
        const std::wstring deviceMac = GetPrimaryMacAddress();
        if (productId.empty() || deviceMac.empty())
        {
            return kRpcInvalidParameter;
        }

        const std::wstring requestBody =
            L"{\"deviceMac\":\"" + JsonEscape(deviceMac) +
            L"\",\"productId\":\"" + JsonEscape(productId) + L"\"}";

        HttpResponse response;
        const long authStatus = AuthorizedPostJson(L"/api/licenses/check", requestBody, response);
        if (authStatus != kRpcSuccess)
        {
            return authStatus;
        }

        if (response.statusCode == kHttpNotFound)
        {
            {
                std::lock_guard<std::mutex> lock(g_StateMutex);
                ClearLicenseStateLocked();
            }
            ClearAvDatabase();
            SignalLicenseWorker();
            return kRpcNotFound;
        }

        if (response.statusCode == kHttpForbidden || response.statusCode == kHttpUnauthorized)
        {
            ClearAuthAndLicenseState();
            return kRpcUnauthorized;
        }

        if (response.statusCode != kHttpOk)
        {
            return kRpcGenericFailure;
        }

        if (!SaveLicenseTicketFromResponse(response.body))
        {
            return kRpcInvalidData;
        }

        if (IsLicenseUnlocked())
        {
            LoadAvDatabaseNow();
            SignalAvUpdateWorker();
        }
        else
        {
            ClearAvDatabase();
        }

        SignalLicenseWorker();
        return kRpcSuccess;
    }

    long ActivateProductNow(const std::wstring& activationCode, std::wstring& errorMessage)
    {
        errorMessage.clear();

        const std::wstring deviceMac = GetPrimaryMacAddress();
        const std::wstring deviceName = GetComputerNameString();
        if (activationCode.empty() || deviceMac.empty())
        {
            errorMessage = L"Activation request validation failed.";
            return kRpcInvalidParameter;
        }

        const std::wstring requestBody =
            L"{\"activationKey\":\"" + JsonEscape(activationCode) +
            L"\",\"deviceMac\":\"" + JsonEscape(deviceMac) +
            L"\",\"deviceName\":\"" + JsonEscape(deviceName) + L"\"}";

        HttpResponse response;
        const long authStatus = AuthorizedPostJson(L"/api/licenses/activate", requestBody, response);
        if (authStatus != kRpcSuccess)
        {
            if (!response.transportOk && response.transportError != ERROR_SUCCESS)
            {
                errorMessage = BuildTransportErrorMessage(L"activate", response.transportError);
            }
            return authStatus;
        }

        if (response.statusCode == kHttpNotFound || response.statusCode == kHttpConflict)
        {
            errorMessage = response.body.empty() ? L"Activation failed." : response.body;
            return kRpcNotFound;
        }

        if (response.statusCode == kHttpForbidden || response.statusCode == kHttpUnauthorized)
        {
            ClearAuthAndLicenseState();
            errorMessage = response.body.empty() ? L"Unauthorized." : response.body;
            return kRpcUnauthorized;
        }

        if (response.statusCode != kHttpOk)
        {
            errorMessage = response.body.empty() ? (L"HTTP status: " + std::to_wstring(response.statusCode)) : response.body;
            return kRpcGenericFailure;
        }

        if (SaveLicenseTicketFromResponse(response.body))
        {
            if (IsLicenseUnlocked())
            {
                LoadAvDatabaseNow();
            }
            else
            {
                ClearAvDatabase();
            }
            SignalLicenseWorker();
            return kRpcSuccess;
        }

        return CheckLicenseNow();
    }

    long LoginUserNow(const std::wstring& username, const std::wstring& password, std::wstring& errorMessage)
    {
        errorMessage.clear();

        if (username.empty() || password.empty())
        {
            errorMessage = L"Username or password is empty.";
            return kRpcInvalidParameter;
        }

        const std::wstring deviceId = L"pc";
        const std::wstring requestBody =
            L"{\"username\":\"" + JsonEscape(username) +
            L"\",\"password\":\"" + JsonEscape(password) +
            L"\",\"deviceId\":\"" + JsonEscape(deviceId) + L"\"}";

        const HttpResponse response = PostJson(L"/api/auth/login", requestBody, L"");
        if (!response.transportOk)
        {
            errorMessage = BuildTransportErrorMessage(L"login", response.transportError);
            return response.transportError != ERROR_SUCCESS ? static_cast<long>(response.transportError) : kRpcGenericFailure;
        }

        if (response.statusCode != kHttpOk)
        {
            errorMessage = response.body.empty() ? (L"HTTP status: " + std::to_wstring(response.statusCode)) : response.body;
            return kRpcLogonFailure;
        }

        std::wstring accessToken;
        std::wstring refreshToken;
        if (!ExtractJsonString(response.body, L"accessToken", accessToken) ||
            !ExtractJsonString(response.body, L"refreshToken", refreshToken))
        {
            errorMessage = response.body.empty() ? L"Login response does not contain tokens." : response.body;
            return kRpcInvalidData;
        }

        SaveAuthTokens(username, accessToken, refreshToken);

        {
            std::lock_guard<std::mutex> lock(g_StateMutex);
            ClearLicenseStateLocked();
        }
        ClearAvDatabase();

        SignalAuthWorker();
        SignalLicenseWorker();
        return kRpcSuccess;
    }

    long LogoutUserNow()
    {
        ClearAuthAndLicenseState();
        return kRpcSuccess;
    }

    long long GetFileWriteStamp(const std::filesystem::path& path)
    {
        std::error_code errorCode;
        const auto stamp = std::filesystem::last_write_time(path, errorCode);
        if (errorCode)
        {
            return 0;
        }
        return stamp.time_since_epoch().count();
    }

    void BuildDirectorySnapshot(const std::wstring& directory, std::map<std::wstring, long long>& snapshot)
    {
        snapshot.clear();
        std::error_code errorCode;
        if (!std::filesystem::exists(directory, errorCode) || !std::filesystem::is_directory(directory, errorCode))
        {
            return;
        }

        const auto options = std::filesystem::directory_options::skip_permission_denied;
        for (std::filesystem::recursive_directory_iterator it(directory, options, errorCode), end; it != end; it.increment(errorCode))
        {
            if (errorCode)
            {
                errorCode.clear();
                continue;
            }

            if (!it->is_regular_file(errorCode))
            {
                errorCode.clear();
                continue;
            }

            snapshot[it->path().wstring()] = GetFileWriteStamp(it->path());
        }
    }

    std::vector<std::wstring> FindNewOrChangedFiles(const std::wstring& directory, std::map<std::wstring, long long>& snapshot)
    {
        std::vector<std::wstring> changedFiles;
        std::map<std::wstring, long long> current;
        BuildDirectorySnapshot(directory, current);

        for (const auto& item : current)
        {
            auto old = snapshot.find(item.first);
            if (old == snapshot.end() || old->second != item.second)
            {
                changedFiles.push_back(item.first);
            }
        }

        snapshot = std::move(current);
        return changedFiles;
    }

    DWORD WINAPI AvUpdateWorkerProc(LPVOID)
    {
        constexpr DWORD kUpdateIntervalMs = 5 * 1000;
        HANDLE waitHandles[2] = { g_StopEvent, g_AvUpdateWorkerEvent };

        while (InterlockedCompareExchange(&g_StopRequested, 0, 0) == 0)
        {
            const DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, kUpdateIntervalMs);
            if (waitResult == WAIT_OBJECT_0)
            {
                break;
            }
            if (waitResult == WAIT_OBJECT_0 + 1 && g_AvUpdateWorkerEvent != nullptr)
            {
                ResetEvent(g_AvUpdateWorkerEvent);
                continue;
            }

            if (waitResult == WAIT_TIMEOUT && IsLicenseUnlocked())
            {
                UpdateAvDatabaseNow(false);
            }
        }

        return 0;
    }

    DWORD WINAPI ScheduledScanWorkerProc(LPVOID)
    {
        HANDLE waitHandles[2] = { g_StopEvent, g_ScheduledScanWorkerEvent };

        while (InterlockedCompareExchange(&g_StopRequested, 0, 0) == 0)
        {
            DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, 1000);
            if (waitResult == WAIT_OBJECT_0)
            {
                break;
            }

            if (waitResult == WAIT_OBJECT_0 + 1 && g_ScheduledScanWorkerEvent != nullptr)
            {
                ResetEvent(g_ScheduledScanWorkerEvent);
            }

            bool shouldRun = false;
            std::wstring path;
            {
                std::lock_guard<std::mutex> lock(g_ScheduledScanMutex);
                const std::time_t now = GetCurrentTimeUtc();
                shouldRun = g_ScheduledScanState.enabled &&
                    !g_ScheduledScanState.path.empty() &&
                    g_ScheduledScanState.intervalMinutes > 0 &&
                    g_ScheduledScanState.nextRunAt != 0 &&
                    now >= g_ScheduledScanState.nextRunAt;
                if (shouldRun)
                {
                    path = g_ScheduledScanState.path;
                    g_ScheduledScanState.nextRunAt = now + static_cast<std::time_t>(g_ScheduledScanState.intervalMinutes) * 60;
                }
            }

            if (!shouldRun)
            {
                continue;
            }

            const std::wstring result = RunScanForPath(path);
            {
                std::lock_guard<std::mutex> lock(g_ScheduledScanMutex);
                g_ScheduledScanState.lastResult = L"Last scheduled scan:\r\n" + result;
            }
        }

        return 0;
    }

    DWORD WINAPI MonitorWorkerProc(LPVOID)
    {
        HANDLE waitHandles[2] = { g_StopEvent, g_MonitorWorkerEvent };

        while (InterlockedCompareExchange(&g_StopRequested, 0, 0) == 0)
        {
            DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, 2000);
            if (waitResult == WAIT_OBJECT_0)
            {
                break;
            }

            if (waitResult == WAIT_OBJECT_0 + 1 && g_MonitorWorkerEvent != nullptr)
            {
                ResetEvent(g_MonitorWorkerEvent);
            }

            bool enabled = false;
            std::wstring directory;
            bool initializeSnapshot = false;
            {
                std::lock_guard<std::mutex> lock(g_MonitorMutex);
                enabled = g_MonitorState.enabled;
                directory = g_MonitorState.directory;
                initializeSnapshot = enabled && !g_MonitorState.snapshotInitialized;
            }

            if (!enabled || directory.empty())
            {
                continue;
            }

            if (initializeSnapshot)
            {
                std::map<std::wstring, long long> snapshot;
                BuildDirectorySnapshot(directory, snapshot);
                std::lock_guard<std::mutex> lock(g_MonitorMutex);
                g_MonitorState.snapshot = std::move(snapshot);
                g_MonitorState.snapshotInitialized = true;
                continue;
            }

            std::vector<std::wstring> changedFiles;
            {
                std::lock_guard<std::mutex> lock(g_MonitorMutex);
                changedFiles = FindNewOrChangedFiles(directory, g_MonitorState.snapshot);
            }

            if (changedFiles.empty())
            {
                continue;
            }

            std::wstringstream stream;
            stream << L"Directory monitoring: " << directory << L"\r\n";
            stream << L"New or changed files detected: " << changedFiles.size() << L"\r\n";
            for (const std::wstring& file : changedFiles)
            {
                stream << L"\r\n" << RunScanForPath(file);
            }

            {
                std::lock_guard<std::mutex> lock(g_MonitorMutex);
                g_MonitorState.lastResult = stream.str();
            }
        }

        return 0;
    }

    DWORD WINAPI AuthWorkerProc(LPVOID)
    {
        HANDLE waitHandles[2] = { g_StopEvent, g_AuthWorkerEvent };

        while (true)
        {
            std::time_t nextRefreshAt = 0;
            {
                std::lock_guard<std::mutex> lock(g_StateMutex);
                if (g_AuthState.authenticated)
                {
                    nextRefreshAt = g_AuthState.nextRefreshAt;
                }
            }

            DWORD waitMs = INFINITE;
            if (nextRefreshAt != 0)
            {
                const std::time_t now = GetCurrentTimeUtc();
                const long long secondsToWait = nextRefreshAt <= now ? 0LL : static_cast<long long>(nextRefreshAt - now);
                const long long waitMs64 = secondsToWait * 1000LL;
                waitMs = waitMs64 > 0x7fffffffLL ? 0x7fffffffUL : static_cast<DWORD>(waitMs64);
            }

            const DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, waitMs);
            if (waitResult == WAIT_OBJECT_0)
            {
                return 0;
            }

            if (waitResult == WAIT_OBJECT_0 + 1)
            {
                ResetEvent(g_AuthWorkerEvent);
                continue;
            }

            if (waitResult == WAIT_TIMEOUT)
            {
                RefreshTokensNow(true);
            }
        }
    }

    DWORD WINAPI LicenseWorkerProc(LPVOID)
    {
        HANDLE waitHandles[2] = { g_StopEvent, g_LicenseWorkerEvent };

        while (true)
        {
            std::time_t nextRefreshAt = 0;
            {
                std::lock_guard<std::mutex> lock(g_StateMutex);
                if (g_LicenseState.hasTicket)
                {
                    nextRefreshAt = g_LicenseState.nextRefreshAt;
                }
            }

            DWORD waitMs = INFINITE;
            if (nextRefreshAt != 0)
            {
                const std::time_t now = GetCurrentTimeUtc();
                const long long secondsToWait = nextRefreshAt <= now ? 0LL : static_cast<long long>(nextRefreshAt - now);
                const long long waitMs64 = secondsToWait * 1000LL;
                waitMs = waitMs64 > 0x7fffffffLL ? 0x7fffffffUL : static_cast<DWORD>(waitMs64);
            }

            const DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, waitMs);
            if (waitResult == WAIT_OBJECT_0)
            {
                return 0;
            }

            if (waitResult == WAIT_OBJECT_0 + 1)
            {
                ResetEvent(g_LicenseWorkerEvent);
                continue;
            }

            if (waitResult == WAIT_TIMEOUT)
            {
                CheckLicenseNow();
            }
        }
    }

    RPC_STATUS StartRpcServer()
    {
        RPC_STATUS status = RpcServerUseProtseqEpW(
            reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(kRpcProtocolSequence)),
            RPC_C_PROTSEQ_MAX_REQS_DEFAULT,
            reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(kRpcEndpoint)),
            nullptr);

        if (status != RPC_S_OK && status != RPC_S_DUPLICATE_ENDPOINT)
        {
            return status;
        }

        status = RpcServerRegisterIf2(
            ServiceControlRpc_v1_0_s_ifspec,
            nullptr,
            nullptr,
            RPC_IF_ALLOW_LOCAL_ONLY,
            RPC_C_LISTEN_MAX_CALLS_DEFAULT,
            static_cast<unsigned int>(-1),
            nullptr);

        if (status != RPC_S_OK && status != RPC_S_TYPE_ALREADY_REGISTERED)
        {
            return status;
        }

        return RpcServerListen(1, RPC_C_LISTEN_MAX_CALLS_DEFAULT, TRUE);
    }

    void StopRpcServer()
    {
        RpcMgmtStopServerListening(nullptr);
        RpcMgmtWaitServerListen();
        RpcServerUnregisterIf(ServiceControlRpc_v1_0_s_ifspec, nullptr, FALSE);
    }

    DWORD WINAPI StopWorker(LPVOID)
    {
        if (InterlockedCompareExchange(&g_StopRequested, 1, 0) != 0)
        {
            return 0;
        }

        SetServiceStatusState(SERVICE_STOP_PENDING, NO_ERROR, 30000);

        if (g_StopEvent != nullptr)
        {
            SetEvent(g_StopEvent);
        }

        SignalAuthWorker();
        SignalLicenseWorker();
        SignalScheduledScanWorker();
        SignalMonitorWorker();
        SignalAvUpdateWorker();
        RpcMgmtStopServerListening(nullptr);
        return 0;
    }

    void QueueServiceStop()
    {
        HANDLE threadHandle = CreateThread(nullptr, 0, StopWorker, nullptr, 0, nullptr);
        if (threadHandle != nullptr)
        {
            CloseHandle(threadHandle);
        }
    }
}

void* __RPC_USER MIDL_user_allocate(size_t size)
{
    return malloc(size);
}

void __RPC_USER MIDL_user_free(void* pointer)
{
    free(pointer);
}

void SetServiceStatusState(DWORD currentState, DWORD win32ExitCode, DWORD waitHint)
{
    g_ServiceStatus.dwCurrentState = currentState;
    g_ServiceStatus.dwWin32ExitCode = win32ExitCode;
    g_ServiceStatus.dwWaitHint = waitHint;

    if (currentState == SERVICE_START_PENDING || currentState == SERVICE_STOP_PENDING)
    {
        g_ServiceStatus.dwControlsAccepted = 0;
    }
    else if (currentState == SERVICE_RUNNING)
    {
        g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_SESSIONCHANGE;
    }
    else
    {
        g_ServiceStatus.dwControlsAccepted = 0;
    }

    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
}

DWORD WINAPI ServiceHandler(DWORD control, DWORD eventType, LPVOID eventData, LPVOID context)
{
    UNREFERENCED_PARAMETER(context);

    if (control == SERVICE_CONTROL_SESSIONCHANGE && eventType == WTS_SESSION_LOGON)
    {
        const auto* notification = static_cast<WTSSESSION_NOTIFICATION*>(eventData);
        if (notification != nullptr)
        {
            LaunchInSession(notification->dwSessionId);
        }
    }

    return NO_ERROR;
}

extern "C" long RpcGetCurrentUser(handle_t bindingHandle, long* isAuthenticated, long userNameBufferLength, wchar_t* userNameBuffer)
{
    UNREFERENCED_PARAMETER(bindingHandle);

    if (isAuthenticated == nullptr || userNameBuffer == nullptr || userNameBufferLength <= 0)
    {
        return kRpcInvalidParameter;
    }

    std::lock_guard<std::mutex> lock(g_StateMutex);
    *isAuthenticated = g_AuthState.authenticated ? 1L : 0L;
    SetOutputString(userNameBuffer, userNameBufferLength, g_AuthState.authenticated ? g_AuthState.username : std::wstring());
    return kRpcSuccess;
}

extern "C" long RpcLogin(handle_t bindingHandle,
    wchar_t* username,
    wchar_t* password,
    long errorMessageBufferLength,
    wchar_t* errorMessageBuffer)
{
    UNREFERENCED_PARAMETER(bindingHandle);

    SetOutputString(errorMessageBuffer, errorMessageBufferLength, L"");

    const std::wstring userValue = username == nullptr ? L"" : TrimCopy(username);
    const std::wstring passwordValue = password == nullptr ? L"" : std::wstring(password);
    std::wstring errorMessage;

    const long status = LoginUserNow(userValue, passwordValue, errorMessage);
    SetOutputString(errorMessageBuffer, errorMessageBufferLength, errorMessage);
    return status;
}

extern "C" long RpcLogout(handle_t bindingHandle)
{
    UNREFERENCED_PARAMETER(bindingHandle);
    return LogoutUserNow();
}

extern "C" long RpcGetLicenseInfo(handle_t bindingHandle,
    long* hasLicense,
    long* isBlocked,
    long expirationDateBufferLength,
    wchar_t* expirationDateBuffer)
{
    UNREFERENCED_PARAMETER(bindingHandle);

    if (hasLicense == nullptr || isBlocked == nullptr || expirationDateBuffer == nullptr || expirationDateBufferLength <= 0)
    {
        return kRpcInvalidParameter;
    }

    bool authenticated = false;
    bool hasTicket = false;
    bool blocked = true;
    std::wstring endingDate;

    {
        std::lock_guard<std::mutex> lock(g_StateMutex);
        authenticated = g_AuthState.authenticated;
        hasTicket = g_LicenseState.hasTicket;
        blocked = g_LicenseState.blocked;
        endingDate = g_LicenseState.endingDate;
    }

    if (!authenticated)
    {
        *hasLicense = 0;
        *isBlocked = 1;
        SetOutputString(expirationDateBuffer, expirationDateBufferLength, L"");
        return kRpcUnauthorized;
    }

    if (!hasTicket)
    {
        const long status = CheckLicenseNow();
        if (status != kRpcSuccess)
        {
            *hasLicense = 0;
            *isBlocked = 1;
            SetOutputString(expirationDateBuffer, expirationDateBufferLength, L"");
            return status;
        }

        std::lock_guard<std::mutex> lock(g_StateMutex);
        hasTicket = g_LicenseState.hasTicket;
        blocked = g_LicenseState.blocked;
        endingDate = g_LicenseState.endingDate;
    }

    *hasLicense = hasTicket ? 1L : 0L;
    *isBlocked = blocked ? 1L : 0L;
    SetOutputString(expirationDateBuffer, expirationDateBufferLength, endingDate);
    return hasTicket ? kRpcSuccess : kRpcNotFound;
}

extern "C" long RpcActivateProduct(handle_t bindingHandle,
    wchar_t* activationCode,
    long errorMessageBufferLength,
    wchar_t* errorMessageBuffer)
{
    UNREFERENCED_PARAMETER(bindingHandle);

    SetOutputString(errorMessageBuffer, errorMessageBufferLength, L"");

    const std::wstring codeValue = activationCode == nullptr ? L"" : TrimCopy(activationCode);
    std::wstring errorMessage;

    const long status = ActivateProductNow(codeValue, errorMessage);
    SetOutputString(errorMessageBuffer, errorMessageBufferLength, errorMessage);
    return status;
}

extern "C" long RpcGetAvDatabaseInfo(handle_t bindingHandle,
    long* isLoaded,
    long* recordCount,
    long releaseDateBufferLength,
    wchar_t* releaseDateBuffer)
{
    UNREFERENCED_PARAMETER(bindingHandle);

    if (isLoaded == nullptr || recordCount == nullptr || releaseDateBuffer == nullptr || releaseDateBufferLength <= 0)
    {
        return kRpcInvalidParameter;
    }

    if (!IsLicenseUnlocked())
    {
        *isLoaded = 0;
        *recordCount = 0;
        SetOutputString(releaseDateBuffer, releaseDateBufferLength, L"");
        return kRpcUnauthorized;
    }

    {
        std::lock_guard<std::mutex> lock(g_AvDatabaseMutex);
        if (!g_AvDatabase.loaded)
        {
            // load below without holding mutex
        }
        else
        {
            *isLoaded = g_AvDatabase.loaded ? 1L : 0L;
            *recordCount = static_cast<long>(g_AvDatabase.recordCount);
            SetOutputString(releaseDateBuffer, releaseDateBufferLength, g_AvDatabase.releaseDate);
            return kRpcSuccess;
        }
    }

    const long loadStatus = LoadAvDatabaseNow();
    if (loadStatus != kRpcSuccess)
    {
        *isLoaded = 0;
        *recordCount = 0;
        SetOutputString(releaseDateBuffer, releaseDateBufferLength, L"");
        return loadStatus;
    }

    std::lock_guard<std::mutex> lock(g_AvDatabaseMutex);
    *isLoaded = g_AvDatabase.loaded ? 1L : 0L;
    *recordCount = static_cast<long>(g_AvDatabase.recordCount);
    SetOutputString(releaseDateBuffer, releaseDateBufferLength, g_AvDatabase.releaseDate);
    return kRpcSuccess;
}

extern "C" long RpcScanFile(handle_t bindingHandle,
    wchar_t* filePath,
    long resultBufferLength,
    wchar_t* resultBuffer)
{
    UNREFERENCED_PARAMETER(bindingHandle);

    if (filePath == nullptr || resultBuffer == nullptr || resultBufferLength <= 0)
    {
        return kRpcInvalidParameter;
    }

    const std::wstring path = TrimCopy(filePath);
    if (path.empty())
    {
        SetOutputString(resultBuffer, resultBufferLength, L"File path is not specified.");
        return kRpcInvalidParameter;
    }

    AvEngine::AvDatabase database;
    const long status = CopyDatabaseForScanning(database);
    if (status != kRpcSuccess)
    {
        SetOutputString(resultBuffer, resultBufferLength, L"Scan is not available. Error code: " + std::to_wstring(status));
        return status;
    }

    SetOutputString(resultBuffer, resultBufferLength, AvEngine::ScanFile(database, path).message);
    return kRpcSuccess;
}

extern "C" long RpcScanDirectory(handle_t bindingHandle,
    wchar_t* directoryPath,
    long resultBufferLength,
    wchar_t* resultBuffer)
{
    UNREFERENCED_PARAMETER(bindingHandle);

    if (directoryPath == nullptr || resultBuffer == nullptr || resultBufferLength <= 0)
    {
        return kRpcInvalidParameter;
    }

    const std::wstring path = TrimCopy(directoryPath);
    if (path.empty())
    {
        SetOutputString(resultBuffer, resultBufferLength, L"Directory path is not specified.");
        return kRpcInvalidParameter;
    }

    AvEngine::AvDatabase database;
    const long status = CopyDatabaseForScanning(database);
    if (status != kRpcSuccess)
    {
        SetOutputString(resultBuffer, resultBufferLength, L"Scan is not available. Error code: " + std::to_wstring(status));
        return status;
    }

    SetOutputString(resultBuffer, resultBufferLength, AvEngine::ScanDirectory(database, path));
    return kRpcSuccess;
}

extern "C" long RpcScanFixedDrives(handle_t bindingHandle,
    long resultBufferLength,
    wchar_t* resultBuffer)
{
    UNREFERENCED_PARAMETER(bindingHandle);

    if (resultBuffer == nullptr || resultBufferLength <= 0)
    {
        return kRpcInvalidParameter;
    }

    AvEngine::AvDatabase database;
    const long status = CopyDatabaseForScanning(database);
    if (status != kRpcSuccess)
    {
        SetOutputString(resultBuffer, resultBufferLength, L"Scan is not available. Error code: " + std::to_wstring(status));
        return status;
    }

    SetOutputString(resultBuffer, resultBufferLength, AvEngine::ScanFixedDrives(database));
    return kRpcSuccess;
}

extern "C" long RpcSetScheduledScan(handle_t bindingHandle,
    long enabled,
    wchar_t* path,
    long intervalMinutes,
    long resultBufferLength,
    wchar_t* resultBuffer)
{
    UNREFERENCED_PARAMETER(bindingHandle);

    if (resultBuffer == nullptr || resultBufferLength <= 0)
    {
        return kRpcInvalidParameter;
    }

    if (enabled == 0)
    {
        std::lock_guard<std::mutex> lock(g_ScheduledScanMutex);
        g_ScheduledScanState.enabled = false;
        g_ScheduledScanState.lastResult = L"Scheduled scan disabled.";
        SetOutputString(resultBuffer, resultBufferLength, g_ScheduledScanState.lastResult);
        SignalScheduledScanWorker();
        return kRpcSuccess;
    }

    if (!IsLicenseUnlocked())
    {
        SetOutputString(resultBuffer, resultBufferLength, L"Scheduled scan setup is not available: license is not active.");
        return kRpcUnauthorized;
    }

    const std::wstring preparedPath = path == nullptr ? L"" : TrimCopy(path);
    if (preparedPath.empty() || intervalMinutes <= 0)
    {
        SetOutputString(resultBuffer, resultBufferLength, L"Specify a path and a positive interval in minutes.");
        return kRpcInvalidParameter;
    }

    {
        std::lock_guard<std::mutex> lock(g_ScheduledScanMutex);
        g_ScheduledScanState.enabled = true;
        g_ScheduledScanState.path = preparedPath;
        g_ScheduledScanState.intervalMinutes = static_cast<DWORD>(intervalMinutes);
        g_ScheduledScanState.nextRunAt = GetCurrentTimeUtc() + static_cast<std::time_t>(intervalMinutes) * 60;
        g_ScheduledScanState.lastResult = L"Scheduled scan configured. First run in " + std::to_wstring(intervalMinutes) + L" min.";
        SetOutputString(resultBuffer, resultBufferLength, g_ScheduledScanState.lastResult);
    }

    SignalScheduledScanWorker();
    return kRpcSuccess;
}

extern "C" long RpcGetScheduledScanResult(handle_t bindingHandle,
    long resultBufferLength,
    wchar_t* resultBuffer)
{
    UNREFERENCED_PARAMETER(bindingHandle);

    if (resultBuffer == nullptr || resultBufferLength <= 0)
    {
        return kRpcInvalidParameter;
    }

    std::lock_guard<std::mutex> lock(g_ScheduledScanMutex);
    SetOutputString(resultBuffer, resultBufferLength,
        g_ScheduledScanState.lastResult.empty() ? L"No scheduled scan results yet." : g_ScheduledScanState.lastResult);
    return kRpcSuccess;
}

extern "C" long RpcSetMonitoredDirectory(handle_t bindingHandle,
    long enabled,
    wchar_t* directoryPath,
    long resultBufferLength,
    wchar_t* resultBuffer)
{
    UNREFERENCED_PARAMETER(bindingHandle);

    if (resultBuffer == nullptr || resultBufferLength <= 0)
    {
        return kRpcInvalidParameter;
    }

    if (enabled == 0)
    {
        std::lock_guard<std::mutex> lock(g_MonitorMutex);
        g_MonitorState.enabled = false;
        g_MonitorState.snapshotInitialized = false;
        g_MonitorState.snapshot.clear();
        g_MonitorState.lastResult = L"Directory monitoring disabled.";
        SetOutputString(resultBuffer, resultBufferLength, g_MonitorState.lastResult);
        SignalMonitorWorker();
        return kRpcSuccess;
    }

    if (!IsLicenseUnlocked())
    {
        SetOutputString(resultBuffer, resultBufferLength, L"Directory monitoring setup is not available: license is not active.");
        return kRpcUnauthorized;
    }

    const std::wstring preparedPath = directoryPath == nullptr ? L"" : TrimCopy(directoryPath);
    if (preparedPath.empty())
    {
        SetOutputString(resultBuffer, resultBufferLength, L"Specify a directory to monitor.");
        return kRpcInvalidParameter;
    }

    {
        std::lock_guard<std::mutex> lock(g_MonitorMutex);
        g_MonitorState.enabled = true;
        g_MonitorState.directory = preparedPath;
        g_MonitorState.snapshotInitialized = false;
        g_MonitorState.snapshot.clear();
        g_MonitorState.lastResult = L"Directory monitoring configured: " + preparedPath;
        SetOutputString(resultBuffer, resultBufferLength, g_MonitorState.lastResult);
    }

    SignalMonitorWorker();
    return kRpcSuccess;
}

extern "C" long RpcGetMonitorResult(handle_t bindingHandle,
    long resultBufferLength,
    wchar_t* resultBuffer)
{
    UNREFERENCED_PARAMETER(bindingHandle);

    if (resultBuffer == nullptr || resultBufferLength <= 0)
    {
        return kRpcInvalidParameter;
    }

    std::lock_guard<std::mutex> lock(g_MonitorMutex);
    SetOutputString(resultBuffer, resultBufferLength,
        g_MonitorState.lastResult.empty() ? L"No monitoring results yet." : g_MonitorState.lastResult);
    return kRpcSuccess;
}

bool IsRpcCallerAdministrator(handle_t bindingHandle)
{
    if (RpcImpersonateClient(bindingHandle) != RPC_S_OK)
    {
        return false;
    }

    HANDLE token = nullptr;
    const BOOL tokenOpened = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY | TOKEN_DUPLICATE, TRUE, &token);
    RevertToSelf();
    if (!tokenOpened || token == nullptr)
    {
        return false;
    }

    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    PSID administratorsSid = nullptr;
    BOOL isMember = FALSE;
    if (AllocateAndInitializeSid(
        &ntAuthority,
        2,
        SECURITY_BUILTIN_DOMAIN_RID,
        DOMAIN_ALIAS_RID_ADMINS,
        0, 0, 0, 0, 0, 0,
        &administratorsSid))
    {
        CheckTokenMembership(token, administratorsSid, &isMember);
        FreeSid(administratorsSid);
    }

    CloseHandle(token);
    return isMember == TRUE;
}

extern "C" void RpcStopService(handle_t bindingHandle)
{
    if (!IsRpcCallerAdministrator(bindingHandle))
    {
        return;
    }

    QueueServiceStop();
}

void WINAPI ServiceMain(DWORD argc, LPWSTR* argv)
{
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    g_StatusHandle = RegisterServiceCtrlHandlerExW(const_cast<LPWSTR>(kServiceName), ServiceHandler, nullptr);
    if (g_StatusHandle == nullptr)
    {
        return;
    }

    ZeroMemory(&g_ServiceStatus, sizeof(g_ServiceStatus));
    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;

    ProtectCurrentServiceProcessFromUserTermination();
    g_ServiceStatus.dwServiceSpecificExitCode = 0;

    SetServiceStatusState(SERVICE_START_PENDING, NO_ERROR, 30000);

    g_StopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_AuthWorkerEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_LicenseWorkerEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_ScheduledScanWorkerEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_MonitorWorkerEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_AvUpdateWorkerEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    if (g_StopEvent == nullptr ||
        g_AuthWorkerEvent == nullptr ||
        g_LicenseWorkerEvent == nullptr ||
        g_ScheduledScanWorkerEvent == nullptr ||
        g_MonitorWorkerEvent == nullptr ||
        g_AvUpdateWorkerEvent == nullptr)
    {
        if (g_AvUpdateWorkerEvent != nullptr)
        {
            CloseHandle(g_AvUpdateWorkerEvent);
            g_AvUpdateWorkerEvent = nullptr;
        }
        if (g_MonitorWorkerEvent != nullptr)
        {
            CloseHandle(g_MonitorWorkerEvent);
            g_MonitorWorkerEvent = nullptr;
        }
        if (g_ScheduledScanWorkerEvent != nullptr)
        {
            CloseHandle(g_ScheduledScanWorkerEvent);
            g_ScheduledScanWorkerEvent = nullptr;
        }
        if (g_LicenseWorkerEvent != nullptr)
        {
            CloseHandle(g_LicenseWorkerEvent);
            g_LicenseWorkerEvent = nullptr;
        }
        if (g_AuthWorkerEvent != nullptr)
        {
            CloseHandle(g_AuthWorkerEvent);
            g_AuthWorkerEvent = nullptr;
        }
        if (g_StopEvent != nullptr)
        {
            CloseHandle(g_StopEvent);
            g_StopEvent = nullptr;
        }

        SetServiceStatusState(SERVICE_STOPPED, GetLastError(), 0);
        return;
    }

    g_AuthWorkerThread = CreateThread(nullptr, 0, AuthWorkerProc, nullptr, 0, nullptr);
    g_LicenseWorkerThread = CreateThread(nullptr, 0, LicenseWorkerProc, nullptr, 0, nullptr);
    g_ScheduledScanWorkerThread = CreateThread(nullptr, 0, ScheduledScanWorkerProc, nullptr, 0, nullptr);
    g_MonitorWorkerThread = CreateThread(nullptr, 0, MonitorWorkerProc, nullptr, 0, nullptr);
    g_AvUpdateWorkerThread = CreateThread(nullptr, 0, AvUpdateWorkerProc, nullptr, 0, nullptr);

    if (g_AuthWorkerThread == nullptr ||
        g_LicenseWorkerThread == nullptr ||
        g_ScheduledScanWorkerThread == nullptr ||
        g_MonitorWorkerThread == nullptr ||
        g_AvUpdateWorkerThread == nullptr)
    {
        if (g_StopEvent != nullptr)
        {
            SetEvent(g_StopEvent);
        }

        if (g_AuthWorkerThread != nullptr)
        {
            CloseHandle(g_AuthWorkerThread);
            g_AuthWorkerThread = nullptr;
        }
        if (g_LicenseWorkerThread != nullptr)
        {
            CloseHandle(g_LicenseWorkerThread);
            g_LicenseWorkerThread = nullptr;
        }
        if (g_ScheduledScanWorkerThread != nullptr)
        {
            CloseHandle(g_ScheduledScanWorkerThread);
            g_ScheduledScanWorkerThread = nullptr;
        }
        if (g_AvUpdateWorkerThread != nullptr)
        {
            CloseHandle(g_AvUpdateWorkerThread);
            g_AvUpdateWorkerThread = nullptr;
        }
        if (g_MonitorWorkerThread != nullptr)
        {
            CloseHandle(g_MonitorWorkerThread);
            g_MonitorWorkerThread = nullptr;
        }

        CloseHandle(g_AvUpdateWorkerEvent);
        CloseHandle(g_MonitorWorkerEvent);
        CloseHandle(g_ScheduledScanWorkerEvent);
        CloseHandle(g_LicenseWorkerEvent);
        CloseHandle(g_AuthWorkerEvent);
        CloseHandle(g_StopEvent);

        g_AvUpdateWorkerEvent = nullptr;
        g_MonitorWorkerEvent = nullptr;
        g_ScheduledScanWorkerEvent = nullptr;
        g_LicenseWorkerEvent = nullptr;
        g_AuthWorkerEvent = nullptr;
        g_StopEvent = nullptr;

        SetServiceStatusState(SERVICE_STOPPED, GetLastError(), 0);
        return;
    }

    const RPC_STATUS rpcStatus = StartRpcServer();
    if (rpcStatus != RPC_S_OK && rpcStatus != RPC_S_ALREADY_LISTENING)
    {
        SetEvent(g_StopEvent);
        SignalAuthWorker();
        SignalLicenseWorker();
        SignalScheduledScanWorker();
        SignalMonitorWorker();
        SignalAvUpdateWorker();
        WaitForSingleObject(g_AuthWorkerThread, 5000);
        WaitForSingleObject(g_LicenseWorkerThread, 5000);
        WaitForSingleObject(g_ScheduledScanWorkerThread, 5000);
        WaitForSingleObject(g_MonitorWorkerThread, 5000);
        WaitForSingleObject(g_AvUpdateWorkerThread, 5000);
        CloseHandle(g_AuthWorkerThread);
        CloseHandle(g_LicenseWorkerThread);
        CloseHandle(g_ScheduledScanWorkerThread);
        CloseHandle(g_MonitorWorkerThread);
        CloseHandle(g_AvUpdateWorkerThread);
        g_AuthWorkerThread = nullptr;
        g_LicenseWorkerThread = nullptr;
        g_ScheduledScanWorkerThread = nullptr;
        g_MonitorWorkerThread = nullptr;
        g_AvUpdateWorkerThread = nullptr;
        CloseHandle(g_AvUpdateWorkerEvent);
        CloseHandle(g_MonitorWorkerEvent);
        CloseHandle(g_ScheduledScanWorkerEvent);
        CloseHandle(g_LicenseWorkerEvent);
        CloseHandle(g_AuthWorkerEvent);
        CloseHandle(g_StopEvent);
        g_AvUpdateWorkerEvent = nullptr;
        g_MonitorWorkerEvent = nullptr;
        g_ScheduledScanWorkerEvent = nullptr;
        g_LicenseWorkerEvent = nullptr;
        g_AuthWorkerEvent = nullptr;
        g_StopEvent = nullptr;
        SetServiceStatusState(SERVICE_STOPPED, rpcStatus, 0);
        return;
    }

    LoadAvDatabaseNow();
    SetServiceStatusState(SERVICE_RUNNING, NO_ERROR, 0);
    LaunchForAllSessions();

    WaitForSingleObject(g_StopEvent, INFINITE);

    SetServiceStatusState(SERVICE_STOP_PENDING, NO_ERROR, 30000);
    TerminateAllLaunchedApplications();
    StopRpcServer();

    SignalAuthWorker();
    SignalLicenseWorker();
    SignalScheduledScanWorker();
    SignalMonitorWorker();
    SignalAvUpdateWorker();

    if (g_AuthWorkerThread != nullptr)
    {
        WaitForSingleObject(g_AuthWorkerThread, 5000);
        CloseHandle(g_AuthWorkerThread);
        g_AuthWorkerThread = nullptr;
    }

    if (g_LicenseWorkerThread != nullptr)
    {
        WaitForSingleObject(g_LicenseWorkerThread, 5000);
        CloseHandle(g_LicenseWorkerThread);
        g_LicenseWorkerThread = nullptr;
    }

    if (g_ScheduledScanWorkerThread != nullptr)
    {
        WaitForSingleObject(g_ScheduledScanWorkerThread, 5000);
        CloseHandle(g_ScheduledScanWorkerThread);
        g_ScheduledScanWorkerThread = nullptr;
    }

    if (g_MonitorWorkerThread != nullptr)
    {
        WaitForSingleObject(g_MonitorWorkerThread, 5000);
        CloseHandle(g_MonitorWorkerThread);
        g_MonitorWorkerThread = nullptr;
    }

    if (g_AvUpdateWorkerThread != nullptr)
    {
        WaitForSingleObject(g_AvUpdateWorkerThread, 5000);
        CloseHandle(g_AvUpdateWorkerThread);
        g_AvUpdateWorkerThread = nullptr;
    }

    if (g_AvUpdateWorkerEvent != nullptr)
    {
        CloseHandle(g_AvUpdateWorkerEvent);
        g_AvUpdateWorkerEvent = nullptr;
    }

    if (g_MonitorWorkerEvent != nullptr)
    {
        CloseHandle(g_MonitorWorkerEvent);
        g_MonitorWorkerEvent = nullptr;
    }

    if (g_ScheduledScanWorkerEvent != nullptr)
    {
        CloseHandle(g_ScheduledScanWorkerEvent);
        g_ScheduledScanWorkerEvent = nullptr;
    }

    if (g_LicenseWorkerEvent != nullptr)
    {
        CloseHandle(g_LicenseWorkerEvent);
        g_LicenseWorkerEvent = nullptr;
    }

    if (g_AuthWorkerEvent != nullptr)
    {
        CloseHandle(g_AuthWorkerEvent);
        g_AuthWorkerEvent = nullptr;
    }

    if (g_StopEvent != nullptr)
    {
        CloseHandle(g_StopEvent);
        g_StopEvent = nullptr;
    }

    SetServiceStatusState(SERVICE_STOPPED, NO_ERROR, 0);
}

int wmain()
{
    SERVICE_TABLE_ENTRYW serviceTable[] =
    {
        { const_cast<LPWSTR>(kServiceName), ServiceMain },
        { nullptr, nullptr }
    };

    if (!StartServiceCtrlDispatcherW(serviceTable))
    {
        return static_cast<int>(GetLastError());
    }

    return 0;
}