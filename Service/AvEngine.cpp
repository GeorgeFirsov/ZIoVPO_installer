#define NOMINMAX

#include "AvEngine.h"

#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <queue>
#include <sstream>
#include <unordered_map>

#pragma comment(lib, "Advapi32.lib")

namespace AvEngine
{
    namespace
    {
        constexpr size_t kPrefixLength = 8;
        constexpr size_t kSha256Length = 32;
        constexpr size_t kMaxTextResultLength = 60000;

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

        std::wstring ToLowerCopy(std::wstring value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch)
                {
                    return static_cast<wchar_t>(towlower(ch));
                });
            return value;
        }

        std::wstring GetExtensionLower(const std::wstring& path)
        {
            std::filesystem::path fsPath(path);
            return ToLowerCopy(fsPath.extension().wstring());
        }

        std::wstring ObjectTypeToString(ObjectType type)
        {
            switch (type)
            {
            case ObjectType::PE:
                return L"PE";
            case ObjectType::PowerShellScript:
                return L"PowerShell Script";
            default:
                return L"Unknown";
            }
        }

        ObjectType ObjectTypeFromServerValue(const std::wstring& value)
        {
            const std::wstring prepared = ToLowerCopy(TrimCopy(value));
            if (prepared == L"pe" || prepared == L"exe" || prepared == L"dll" || prepared == L"application/x-msdownload")
            {
                return ObjectType::PE;
            }

            if (prepared == L"ps1" || prepared == L"powershell" || prepared == L"powershellscript" || prepared == L"powershell script")
            {
                return ObjectType::PowerShellScript;
            }

            return ObjectType::Unknown;
        }

        ObjectType DetectObjectType(const std::wstring& filePath, const std::vector<unsigned char>& bytes)
        {
            if (bytes.size() >= 2 && bytes[0] == 'M' && bytes[1] == 'Z')
            {
                return ObjectType::PE;
            }

            const std::wstring extension = GetExtensionLower(filePath);
            if (extension == L".exe" || extension == L".dll" || extension == L".sys")
            {
                return ObjectType::PE;
            }

            if (extension == L".ps1" || extension == L".psm1" || extension == L".psd1")
            {
                return ObjectType::PowerShellScript;
            }

            return ObjectType::Unknown;
        }

        bool ReadFileBytes(const std::wstring& filePath, std::vector<unsigned char>& bytes)
        {
            std::ifstream file(std::filesystem::path(filePath), std::ios::binary);
            if (!file.is_open())
            {
                return false;
            }

            file.seekg(0, std::ios::end);
            const std::streamoff length = file.tellg();
            if (length < 0)
            {
                return false;
            }
            file.seekg(0, std::ios::beg);

            bytes.resize(static_cast<size_t>(length));
            if (!bytes.empty())
            {
                file.read(reinterpret_cast<char*>(bytes.data()), length);
            }

            return file.good() || file.eof();
        }

        int HexValue(wchar_t ch)
        {
            if (ch >= L'0' && ch <= L'9') return static_cast<int>(ch - L'0');
            if (ch >= L'a' && ch <= L'f') return static_cast<int>(ch - L'a') + 10;
            if (ch >= L'A' && ch <= L'F') return static_cast<int>(ch - L'A') + 10;
            return -1;
        }

        bool HexToBytes(const std::wstring& hex, std::vector<unsigned char>& bytes)
        {
            const std::wstring prepared = TrimCopy(hex);
            if (prepared.size() % 2 != 0)
            {
                return false;
            }

            bytes.clear();
            bytes.reserve(prepared.size() / 2);
            for (size_t index = 0; index < prepared.size(); index += 2)
            {
                const int high = HexValue(prepared[index]);
                const int low = HexValue(prepared[index + 1]);
                if (high < 0 || low < 0)
                {
                    return false;
                }
                bytes.push_back(static_cast<unsigned char>((high << 4) | low));
            }
            return true;
        }

        uint64_t BytesToPrefix(const std::vector<unsigned char>& bytes)
        {
            uint64_t value = 0;
            for (size_t index = 0; index < kPrefixLength && index < bytes.size(); ++index)
            {
                value = (value << 8) | static_cast<uint64_t>(bytes[index]);
            }
            return value;
        }

        std::vector<unsigned char> PrefixToBytes(uint64_t prefix)
        {
            std::vector<unsigned char> result(kPrefixLength);
            for (size_t index = 0; index < kPrefixLength; ++index)
            {
                result[kPrefixLength - index - 1] = static_cast<unsigned char>(prefix & 0xFF);
                prefix >>= 8;
            }
            return result;
        }

        std::vector<unsigned char> Base64Decode(const std::wstring& value)
        {
            std::string input;
            input.reserve(value.size());

            for (wchar_t ch : value)
            {
                if (ch <= 0x7F && !iswspace(ch))
                {
                    if (ch == L'-') input.push_back('+');
                    else if (ch == L'_') input.push_back('/');
                    else input.push_back(static_cast<char>(ch));
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

        std::vector<unsigned char> Sha256(const unsigned char* data, size_t size)
        {
            HCRYPTPROV provider = 0;
            HCRYPTHASH hash = 0;
            std::vector<unsigned char> digest(kSha256Length);

            if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
            {
                return {};
            }

            if (!CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash))
            {
                CryptReleaseContext(provider, 0);
                return {};
            }

            bool ok = true;
            if (size > 0)
            {
                ok = CryptHashData(hash, data, static_cast<DWORD>(size), 0) == TRUE;
            }

            DWORD digestSize = static_cast<DWORD>(digest.size());
            if (ok)
            {
                ok = CryptGetHashParam(hash, HP_HASHVAL, digest.data(), &digestSize, 0) == TRUE;
            }

            CryptDestroyHash(hash);
            CryptReleaseContext(provider, 0);

            if (!ok || digestSize != kSha256Length)
            {
                return {};
            }

            return digest;
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

        std::vector<std::wstring> SplitJsonObjects(const std::wstring& json)
        {
            std::vector<std::wstring> objects;
            bool inString = false;
            bool escaped = false;
            int depth = 0;
            size_t objectStart = std::wstring::npos;

            for (size_t index = 0; index < json.size(); ++index)
            {
                const wchar_t ch = json[index];

                if (inString)
                {
                    if (escaped)
                    {
                        escaped = false;
                    }
                    else if (ch == L'\\')
                    {
                        escaped = true;
                    }
                    else if (ch == L'\"')
                    {
                        inString = false;
                    }
                    continue;
                }

                if (ch == L'\"')
                {
                    inString = true;
                    continue;
                }

                if (ch == L'{')
                {
                    if (depth == 0)
                    {
                        objectStart = index;
                    }
                    ++depth;
                }
                else if (ch == L'}')
                {
                    --depth;
                    if (depth == 0 && objectStart != std::wstring::npos)
                    {
                        objects.push_back(json.substr(objectStart, index - objectStart + 1));
                        objectStart = std::wstring::npos;
                    }
                }
            }

            return objects;
        }

        void AddPatternToTrie(std::vector<AhoNode>& trie, const std::vector<unsigned char>& pattern, uint64_t prefix)
        {
            size_t node = 0;
            for (unsigned char byte : pattern)
            {
                auto found = trie[node].next.find(byte);
                if (found == trie[node].next.end())
                {
                    const size_t newNode = trie.size();
                    trie[node].next[byte] = newNode;
                    trie.emplace_back();
                    node = newNode;
                }
                else
                {
                    node = found->second;
                }
            }
            trie[node].outputPrefixes.push_back(prefix);
        }

        void BuildAhoTrie(AvDatabase& database)
        {
            database.ahoTrie.clear();
            database.ahoTrie.emplace_back();

            for (const auto& item : database.recordsByPrefix)
            {
                AddPatternToTrie(database.ahoTrie, PrefixToBytes(item.first), item.first);
            }

            std::queue<size_t> queue;
            for (const auto& item : database.ahoTrie[0].next)
            {
                database.ahoTrie[item.second].fail = 0;
                queue.push(item.second);
            }

            while (!queue.empty())
            {
                const size_t current = queue.front();
                queue.pop();

                for (const auto& transition : database.ahoTrie[current].next)
                {
                    const unsigned char byte = transition.first;
                    const size_t child = transition.second;
                    size_t fail = database.ahoTrie[current].fail;

                    while (fail != 0 && database.ahoTrie[fail].next.find(byte) == database.ahoTrie[fail].next.end())
                    {
                        fail = database.ahoTrie[fail].fail;
                    }

                    auto found = database.ahoTrie[fail].next.find(byte);
                    if (found != database.ahoTrie[fail].next.end() && found->second != child)
                    {
                        database.ahoTrie[child].fail = found->second;
                    }
                    else
                    {
                        database.ahoTrie[child].fail = 0;
                    }

                    const auto& inheritedOutput = database.ahoTrie[database.ahoTrie[child].fail].outputPrefixes;
                    database.ahoTrie[child].outputPrefixes.insert(database.ahoTrie[child].outputPrefixes.end(), inheritedOutput.begin(), inheritedOutput.end());
                    queue.push(child);
                }
            }
        }

        std::vector<std::pair<size_t, uint64_t>> SearchPrefixes(const AvDatabase& database, const std::vector<unsigned char>& bytes)
        {
            std::vector<std::pair<size_t, uint64_t>> hits;
            if (database.ahoTrie.empty() || bytes.empty())
            {
                return hits;
            }

            size_t node = 0;
            for (size_t index = 0; index < bytes.size(); ++index)
            {
                const unsigned char byte = bytes[index];
                while (node != 0 && database.ahoTrie[node].next.find(byte) == database.ahoTrie[node].next.end())
                {
                    node = database.ahoTrie[node].fail;
                }

                auto found = database.ahoTrie[node].next.find(byte);
                if (found != database.ahoTrie[node].next.end())
                {
                    node = found->second;
                }

                for (uint64_t prefix : database.ahoTrie[node].outputPrefixes)
                {
                    if (index + 1 >= kPrefixLength)
                    {
                        hits.emplace_back(index + 1 - kPrefixLength, prefix);
                    }
                }
            }

            return hits;
        }

        void AppendLimited(std::wstringstream& stream, const std::wstring& value)
        {
            if (stream.tellp() >= static_cast<std::streampos>(kMaxTextResultLength))
            {
                return;
            }
            stream << value;
        }
    }

    bool BuildDatabaseFromJson(const std::wstring& json, AvDatabase& database, std::wstring& errorMessage)
    {
        database = {};
        database.loaded = true;

        const std::vector<std::wstring> objects = SplitJsonObjects(json);
        for (const std::wstring& objectJson : objects)
        {
            std::wstring firstBytesHex;
            std::wstring remainderHashHex;
            std::wstring fileType;
            std::wstring digitalSignatureBase64;
            std::wstring updatedAt;
            std::wstring threatName;
            long long remainderLength = 0;
            long long offsetStart = 0;
            long long offsetEnd = 0;

            if (!ExtractJsonRawValue(objectJson, L"firstBytesHex", firstBytesHex) ||
                !ExtractJsonRawValue(objectJson, L"remainderHashHex", remainderHashHex) ||
                !ExtractJsonLongLong(objectJson, L"remainderLength", remainderLength) ||
                !ExtractJsonRawValue(objectJson, L"fileType", fileType) ||
                !ExtractJsonLongLong(objectJson, L"offsetStart", offsetStart) ||
                !ExtractJsonLongLong(objectJson, L"offsetEnd", offsetEnd))
            {
                errorMessage = L"Failed to parse one antivirus database record.";
                return false;
            }

            ExtractJsonRawValue(objectJson, L"digitalSignatureBase64", digitalSignatureBase64);
            ExtractJsonRawValue(objectJson, L"updatedAt", updatedAt);
            ExtractJsonRawValue(objectJson, L"threatName", threatName);

            std::vector<unsigned char> prefixBytes;
            std::vector<unsigned char> objectHash;
            if (!HexToBytes(firstBytesHex, prefixBytes) || prefixBytes.size() != kPrefixLength)
            {
                errorMessage = L"Signature prefix must contain exactly 8 bytes.";
                return false;
            }
            if (!HexToBytes(remainderHashHex, objectHash) || objectHash.size() != kSha256Length)
            {
                errorMessage = L"Signature hash must be SHA-256.";
                return false;
            }
            if (remainderLength < 0 || offsetStart < 0 || offsetEnd < offsetStart)
            {
                errorMessage = L"Invalid numeric signature fields.";
                return false;
            }

            AvRecord record;
            record.objectSignaturePrefix = BytesToPrefix(prefixBytes);
            record.objectSignatureLength = static_cast<uint32_t>(kPrefixLength + static_cast<size_t>(remainderLength));
            record.objectSignature = objectHash;
            record.offsetBegin = static_cast<uint64_t>(offsetStart);
            record.offsetEnd = static_cast<uint64_t>(offsetEnd);
            record.objectType = ObjectTypeFromServerValue(fileType);
            record.avRecordSignature = Base64Decode(digitalSignatureBase64);
            record.threatName = threatName.empty() ? L"unknown" : threatName;

            if (record.objectType == ObjectType::Unknown)
            {
                continue;
            }

            database.recordsByPrefix[record.objectSignaturePrefix].push_back(record);
            ++database.recordCount;

            if (!updatedAt.empty() && updatedAt > database.releaseDate)
            {
                database.releaseDate = updatedAt;
            }
        }

        if (database.releaseDate.empty())
        {
            database.releaseDate = L"-";
        }

        BuildAhoTrie(database);
        return true;
    }


    bool BuildDatabaseFromRecords(const std::vector<AvRecord>& records, const std::wstring& releaseDate, AvDatabase& database, std::wstring& errorMessage)
    {
        database = {};
        database.loaded = true;
        database.releaseDate = releaseDate.empty() ? L"-" : releaseDate;

        for (const AvRecord& record : records)
        {
            if (record.objectType == ObjectType::Unknown)
            {
                continue;
            }
            if (record.objectSignatureLength < kPrefixLength || record.objectSignature.size() != kSha256Length)
            {
                errorMessage = L"Antivirus database record has invalid signature fields.";
                database = {};
                return false;
            }

            database.recordsByPrefix[record.objectSignaturePrefix].push_back(record);
            ++database.recordCount;
        }

        BuildAhoTrie(database);
        return true;
    }

    FileScanResult ScanFile(const AvDatabase& database, const std::wstring& filePath)
    {
        FileScanResult result;
        result.filePath = filePath;

        if (!database.loaded)
        {
            result.message = L"Antivirus database is not loaded.";
            return result;
        }

        std::vector<unsigned char> bytes;
        if (!ReadFileBytes(filePath, bytes))
        {
            result.message = L"Failed to open file: " + filePath;
            return result;
        }

        result.scanCompleted = true;
        if (bytes.size() < kPrefixLength || database.recordCount == 0)
        {
            result.message = L"File: " + filePath + L"\r\nResult: no threats found\r\n";
            return result;
        }

        const ObjectType detectedType = DetectObjectType(filePath, bytes);
        const auto hits = SearchPrefixes(database, bytes);

        for (const auto& hit : hits)
        {
            const size_t position = hit.first;
            const uint64_t prefix = hit.second;
            auto recordsIt = database.recordsByPrefix.find(prefix);
            if (recordsIt == database.recordsByPrefix.end())
            {
                continue;
            }

            for (const AvRecord& record : recordsIt->second)
            {
                if (record.objectType != detectedType)
                {
                    continue;
                }

                if (position < record.offsetBegin || position > record.offsetEnd)
                {
                    continue;
                }

                if (record.objectSignatureLength < kPrefixLength || position + record.objectSignatureLength > bytes.size())
                {
                    continue;
                }

                const std::vector<unsigned char> hash = Sha256(bytes.data() + position, record.objectSignatureLength);
                if (hash.empty())
                {
                    continue;
                }

                if (hash == record.objectSignature)
                {
                    result.malicious = true;
                    result.threatName = record.threatName;
                    result.objectType = ObjectTypeToString(record.objectType);
                    result.position = static_cast<uint64_t>(position);

                    std::wstringstream stream;
                    stream << L"File: " << filePath << L"\r\n";
                    stream << L"Result: threat detected\r\n";
                    stream << L"Threat: " << result.threatName << L"\r\n";
                    stream << L"Object type: " << result.objectType << L"\r\n";
                    stream << L"Position: " << result.position << L"\r\n";
                    result.message = stream.str();
                    return result;
                }
            }
        }

        result.message = L"File: " + filePath + L"\r\nResult: no threats found\r\n";
        return result;
    }

    std::wstring ScanDirectory(const AvDatabase& database, const std::wstring& directoryPath)
    {
        std::wstringstream stream;
        stream << L"Directory scan: " << directoryPath << L"\r\n";

        std::error_code errorCode;
        if (!std::filesystem::exists(directoryPath, errorCode) || !std::filesystem::is_directory(directoryPath, errorCode))
        {
            stream << L"Result: directory not found\r\n";
            return stream.str();
        }

        size_t scanned = 0;
        size_t infected = 0;
        size_t errors = 0;

        const auto options = std::filesystem::directory_options::skip_permission_denied;
        for (std::filesystem::recursive_directory_iterator it(directoryPath, options, errorCode), end; it != end; it.increment(errorCode))
        {
            if (errorCode)
            {
                ++errors;
                errorCode.clear();
                continue;
            }

            if (!it->is_regular_file(errorCode))
            {
                errorCode.clear();
                continue;
            }

            ++scanned;
            FileScanResult result = ScanFile(database, it->path().wstring());
            if (!result.scanCompleted)
            {
                ++errors;
                continue;
            }
            if (result.malicious)
            {
                ++infected;
                AppendLimited(stream, L"\r\n" + result.message);
            }
        }

        stream << L"\r\nTotal files scanned: " << scanned << L"\r\n";
        stream << L"Threats found: " << infected << L"\r\n";
        stream << L"Access/read errors: " << errors << L"\r\n";
        if (infected == 0)
        {
            stream << L"Result: no threats found\r\n";
        }
        return stream.str();
    }

    std::wstring ScanFixedDrives(const AvDatabase& database)
    {
        std::wstringstream stream;
        stream << L"Fixed drives scan\r\n";

        const DWORD drivesMask = GetLogicalDrives();
        if (drivesMask == 0)
        {
            stream << L"Failed to get drive list.\r\n";
            return stream.str();
        }

        bool foundDrive = false;
        for (wchar_t letter = L'A'; letter <= L'Z'; ++letter)
        {
            const int bit = letter - L'A';
            if ((drivesMask & (1u << bit)) == 0)
            {
                continue;
            }

            wchar_t root[] = { letter, L':', L'\\', L'\0' };
            if (GetDriveTypeW(root) != DRIVE_FIXED)
            {
                continue;
            }

            foundDrive = true;
            AppendLimited(stream, L"\r\n" + ScanDirectory(database, root));
        }

        if (!foundDrive)
        {
            stream << L"No fixed drives found.\r\n";
        }

        return stream.str();
    }

    std::wstring ScanPath(const AvDatabase& database, const std::wstring& path)
    {
        std::error_code errorCode;
        if (std::filesystem::is_regular_file(path, errorCode))
        {
            return ScanFile(database, path).message;
        }

        errorCode.clear();
        if (std::filesystem::is_directory(path, errorCode))
        {
            return ScanDirectory(database, path);
        }

        return L"Path not found or not a file/directory: " + path + L"\r\n";
    }
}
