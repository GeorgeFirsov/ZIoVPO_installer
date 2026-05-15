#pragma once

#include <windows.h>

#include <string>

inline constexpr wchar_t kServiceName[] = L"ZIoVPO_service";
inline constexpr wchar_t kRpcProtocolSequence[] = L"ncalrpc";
inline constexpr wchar_t kRpcEndpoint[] = L"ZIoVPO_ServiceControl";
inline constexpr wchar_t kApplicationPathFile[] = L"%PUBLIC%\\Documents\\ZIoVPO_ApplicationPath.txt";

struct ServiceUserInfo
{
    DWORD statusCode = ERROR_GEN_FAILURE;
    bool authenticated = false;
    std::wstring username;
};

struct ServiceLicenseInfo
{
    DWORD statusCode = ERROR_GEN_FAILURE;
    bool hasLicense = false;
    bool blocked = true;
    std::wstring expirationDate;
};

struct ServiceAvDatabaseInfo
{
    DWORD statusCode = ERROR_GEN_FAILURE;
    bool loaded = false;
    long recordCount = 0;
    std::wstring releaseDate;
};

bool EnsureServiceRunningAndWait(const wchar_t* serviceName, DWORD timeoutMs);
bool RequestServiceStart();
bool IsStartedByService(const wchar_t* serviceName);
bool RequestServiceStop();
bool ProtectCurrentProcessFromUserTermination();
bool IsSecureStopCommandLine(LPCWSTR commandLine);
bool IsSecureStartCommandLine(LPCWSTR commandLine);
void SaveApplicationPathForService();

bool GetCurrentUserInfo(ServiceUserInfo& info);
bool RequestLogin(const std::wstring& username, const std::wstring& password, DWORD& statusCode, std::wstring& errorMessage);
bool RequestLogout(DWORD& statusCode);
bool GetCurrentLicenseInfo(ServiceLicenseInfo& info);
bool RequestProductActivation(const std::wstring& activationCode, DWORD& statusCode, std::wstring& errorMessage);
bool GetAvDatabaseInfo(ServiceAvDatabaseInfo& info);
bool RequestFileScan(const std::wstring& filePath, DWORD& statusCode, std::wstring& resultText);
bool RequestDirectoryScan(const std::wstring& directoryPath, DWORD& statusCode, std::wstring& resultText);
bool RequestFixedDrivesScan(DWORD& statusCode, std::wstring& resultText);
bool ConfigureScheduledScan(bool enabled, const std::wstring& path, long intervalMinutes, DWORD& statusCode, std::wstring& resultText);
bool GetScheduledScanResult(DWORD& statusCode, std::wstring& resultText);
bool ConfigureMonitoredDirectory(bool enabled, const std::wstring& directoryPath, DWORD& statusCode, std::wstring& resultText);
bool GetMonitorResult(DWORD& statusCode, std::wstring& resultText);