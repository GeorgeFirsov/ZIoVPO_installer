#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace AvEngine
{
    enum class ObjectType : uint32_t
    {
        Unknown = 0,
        PE = 1,
        PowerShellScript = 2
    };

    struct AvRecord
    {
        uint64_t objectSignaturePrefix = 0;
        uint32_t objectSignatureLength = 0;
        std::vector<unsigned char> objectSignature;
        uint64_t offsetBegin = 0;
        uint64_t offsetEnd = 0;
        ObjectType objectType = ObjectType::Unknown;
        std::vector<unsigned char> avRecordSignature;
        std::wstring threatName;
    };

    struct AhoNode
    {
        std::map<unsigned char, size_t> next;
        size_t fail = 0;
        std::vector<uint64_t> outputPrefixes;
    };

    struct AvDatabase
    {
        bool loaded = false;
        std::wstring releaseDate;
        size_t recordCount = 0;
        std::map<uint64_t, std::vector<AvRecord>> recordsByPrefix;
        std::vector<AhoNode> ahoTrie;
    };

    struct FileScanResult
    {
        bool scanCompleted = false;
        bool malicious = false;
        std::wstring message;
        std::wstring filePath;
        std::wstring threatName;
        std::wstring objectType;
        uint64_t position = 0;
    };

    bool BuildDatabaseFromJson(const std::wstring& json, AvDatabase& database, std::wstring& errorMessage);
    bool BuildDatabaseFromRecords(const std::vector<AvRecord>& records, const std::wstring& releaseDate, AvDatabase& database, std::wstring& errorMessage);
    FileScanResult ScanFile(const AvDatabase& database, const std::wstring& filePath);
    std::wstring ScanDirectory(const AvDatabase& database, const std::wstring& directoryPath);
    std::wstring ScanFixedDrives(const AvDatabase& database);
    std::wstring ScanPath(const AvDatabase& database, const std::wstring& path);
}
