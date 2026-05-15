#define NOMINMAX

#include "AvDatabaseStorage.h"
#include "SignatureVerifier.h"

#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#pragma comment(lib, "Advapi32.lib")

namespace AvDatabaseStorage
{
    namespace
    {
        constexpr unsigned char kManifestMagic[4] = { 'M', 'F', '-', 'F' };
        constexpr unsigned char kDataMagic[4] = { 'D', 'B', '-', 'F' };
        constexpr uint16_t kFormatVersion = 1;
        constexpr size_t kDataHeaderSize = 10;
        constexpr size_t kSha256Length = 32;
        constexpr size_t kPrefixLength = 8;

        struct DataReader
        {
            const std::vector<unsigned char>& bytes;
            size_t pos = 0;

            bool CanRead(size_t count) const
            {
                return count <= bytes.size() && pos <= bytes.size() - count;
            }

            bool ReadU8(uint8_t& value)
            {
                if (!CanRead(1)) return false;
                value = bytes[pos++];
                return true;
            }

            bool ReadU16(uint16_t& value)
            {
                if (!CanRead(2)) return false;
                value = (static_cast<uint16_t>(bytes[pos]) << 8) |
                    static_cast<uint16_t>(bytes[pos + 1]);
                pos += 2;
                return true;
            }

            bool ReadU32(uint32_t& value)
            {
                if (!CanRead(4)) return false;
                value = (static_cast<uint32_t>(bytes[pos]) << 24) |
                    (static_cast<uint32_t>(bytes[pos + 1]) << 16) |
                    (static_cast<uint32_t>(bytes[pos + 2]) << 8) |
                    static_cast<uint32_t>(bytes[pos + 3]);
                pos += 4;
                return true;
            }

            bool ReadI64(int64_t& value)
            {
                if (!CanRead(8)) return false;
                uint64_t unsignedValue = 0;
                for (int i = 0; i < 8; ++i)
                {
                    unsignedValue = (unsignedValue << 8) | static_cast<uint64_t>(bytes[pos + i]);
                }
                pos += 8;
                value = static_cast<int64_t>(unsignedValue);
                return true;
            }

            bool ReadU64(uint64_t& value)
            {
                int64_t signedValue = 0;
                if (!ReadI64(signedValue) || signedValue < 0) return false;
                value = static_cast<uint64_t>(signedValue);
                return true;
            }

            bool ReadBytes(size_t count, std::vector<unsigned char>& value)
            {
                if (!CanRead(count)) return false;
                value.assign(bytes.begin() + static_cast<std::ptrdiff_t>(pos), bytes.begin() + static_cast<std::ptrdiff_t>(pos + count));
                pos += count;
                return true;
            }
        };

        struct ManifestRecord
        {
            std::wstring id;
            uint8_t status = 0;
            int64_t updatedAt = 0;
            uint64_t dataOffset = 0;
            uint32_t dataLength = 0;
            std::vector<unsigned char> recordSignature;
        };

        std::wstring JoinPath(const std::wstring& left, const std::wstring& right)
        {
            if (left.empty()) return right;
            std::filesystem::path path(left);
            path /= right;
            return path.wstring();
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

        bool WriteFileBytes(const std::wstring& filePath, const std::vector<unsigned char>& bytes)
        {
            std::ofstream file(std::filesystem::path(filePath), std::ios::binary | std::ios::trunc);
            if (!file.is_open())
            {
                return false;
            }
            if (!bytes.empty())
            {
                file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            }
            return file.good();
        }

        std::vector<unsigned char> Sha256(const std::vector<unsigned char>& data)
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
            if (!data.empty())
            {
                ok = CryptHashData(hash, data.data(), static_cast<DWORD>(data.size()), 0) == TRUE;
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

        std::wstring Utf8ToWide(const std::vector<unsigned char>& bytes)
        {
            if (bytes.empty())
            {
                return L"";
            }
            const int required = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), nullptr, 0);
            if (required <= 0)
            {
                return L"";
            }
            std::wstring wide(required, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), wide.data(), required);
            return wide;
        }

        std::string WideToUtf8(const std::wstring& value)
        {
            if (value.empty())
            {
                return {};
            }
            const int required = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
            if (required <= 0)
            {
                return {};
            }
            std::string result(required, '\0');
            WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), required, nullptr, nullptr);
            return result;
        }

        std::wstring BytesToHexWide(const std::vector<unsigned char>& bytes)
        {
            static constexpr wchar_t hex[] = L"0123456789ABCDEF";
            std::wstring result;
            result.reserve(bytes.size() * 2);
            for (unsigned char byte : bytes)
            {
                result.push_back(hex[(byte >> 4) & 0x0F]);
                result.push_back(hex[byte & 0x0F]);
            }
            return result;
        }

        std::wstring JsonEscape(const std::wstring& value)
        {
            std::wstring result;
            for (wchar_t ch : value)
            {
                switch (ch)
                {
                case L'\\': result += L"\\\\"; break;
                case L'\"': result += L"\\\""; break;
                case L'\b': result += L"\\b"; break;
                case L'\t': result += L"\\t"; break;
                case L'\n': result += L"\\n"; break;
                case L'\f': result += L"\\f"; break;
                case L'\r': result += L"\\r"; break;
                default:
                    if (ch >= 0 && ch <= 0x1F)
                    {
                        wchar_t buffer[7] = {};
                        swprintf_s(buffer, L"\\u%04x", static_cast<unsigned int>(ch));
                        result += buffer;
                    }
                    else
                    {
                        result.push_back(ch);
                    }
                    break;
                }
            }
            return result;
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

        AvEngine::ObjectType ObjectTypeFromText(const std::wstring& value)
        {
            std::wstring prepared = value;
            std::transform(prepared.begin(), prepared.end(), prepared.begin(), [](wchar_t ch)
                {
                    return static_cast<wchar_t>(towlower(ch));
                });
            if (prepared == L"pe" || prepared == L"exe" || prepared == L"dll" || prepared == L"application/x-msdownload")
            {
                return AvEngine::ObjectType::PE;
            }
            if (prepared == L"ps1" || prepared == L"powershell" || prepared == L"powershellscript" || prepared == L"powershell script")
            {
                return AvEngine::ObjectType::PowerShellScript;
            }
            return AvEngine::ObjectType::Unknown;
        }

        std::wstring FormatReleaseDate(int64_t epochMillis)
        {
            if (epochMillis <= 0)
            {
                return L"-";
            }
            const std::time_t seconds = static_cast<std::time_t>(epochMillis / 1000);
            std::tm utcTime = {};
            gmtime_s(&utcTime, &seconds);
            wchar_t buffer[64] = {};
            wcsftime(buffer, ARRAYSIZE(buffer), L"%Y-%m-%dT%H:%M:%SZ", &utcTime);
            return buffer;
        }

        std::wstring GuidFromBytes(const std::vector<unsigned char>& bytes)
        {
            if (bytes.size() != 16)
            {
                return L"";
            }
            wchar_t buffer[40] = {};
            swprintf_s(buffer,
                L"%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                bytes[0], bytes[1], bytes[2], bytes[3],
                bytes[4], bytes[5],
                bytes[6], bytes[7],
                bytes[8], bytes[9],
                bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
            return buffer;
        }

        bool ReadUtf8(DataReader& reader, std::wstring& value)
        {
            uint32_t length = 0;
            if (!reader.ReadU32(length) || length > 1024 * 1024)
            {
                return false;
            }
            std::vector<unsigned char> bytes;
            if (!reader.ReadBytes(length, bytes))
            {
                return false;
            }
            value = Utf8ToWide(bytes);
            return true;
        }

        bool ReadLengthPrefixedBytes(DataReader& reader, std::vector<unsigned char>& value)
        {
            uint32_t length = 0;
            if (!reader.ReadU32(length) || length > 1024 * 1024)
            {
                return false;
            }
            return reader.ReadBytes(length, value);
        }

        bool ParseDataRecord(const std::vector<unsigned char>& dataBytes, const ManifestRecord& manifestRecord, AvEngine::AvRecord& record)
        {
            if (manifestRecord.dataLength == 0 || manifestRecord.dataLength > 32 * 1024 * 1024)
            {
                return false;
            }
            const uint64_t absoluteOffset = static_cast<uint64_t>(kDataHeaderSize) + manifestRecord.dataOffset;
            if (absoluteOffset > dataBytes.size() || manifestRecord.dataLength > dataBytes.size() - static_cast<size_t>(absoluteOffset))
            {
                return false;
            }

            std::vector<unsigned char> recordBytes(
                dataBytes.begin() + static_cast<std::ptrdiff_t>(absoluteOffset),
                dataBytes.begin() + static_cast<std::ptrdiff_t>(absoluteOffset + manifestRecord.dataLength));
            DataReader reader{ recordBytes };

            std::wstring threatName;
            std::vector<unsigned char> prefixBytes;
            std::vector<unsigned char> remainderHashBytes;
            int64_t remainderLength = 0;
            std::wstring objectTypeText;
            int64_t offsetStart = 0;
            int64_t offsetEnd = 0;

            if (!ReadUtf8(reader, threatName) ||
                !ReadLengthPrefixedBytes(reader, prefixBytes) ||
                !ReadLengthPrefixedBytes(reader, remainderHashBytes) ||
                !reader.ReadI64(remainderLength) ||
                !ReadUtf8(reader, objectTypeText) ||
                !reader.ReadI64(offsetStart) ||
                !reader.ReadI64(offsetEnd) ||
                reader.pos != recordBytes.size())
            {
                return false;
            }

            if (prefixBytes.size() != kPrefixLength || remainderHashBytes.size() != kSha256Length || remainderLength < 0 || offsetStart < 0 || offsetEnd < offsetStart)
            {
                return false;
            }

            record = {};
            record.objectSignaturePrefix = BytesToPrefix(prefixBytes);
            record.objectSignatureLength = static_cast<uint32_t>(kPrefixLength + static_cast<size_t>(remainderLength));
            record.objectSignature = remainderHashBytes;
            record.offsetBegin = static_cast<uint64_t>(offsetStart);
            record.offsetEnd = static_cast<uint64_t>(offsetEnd);
            record.objectType = ObjectTypeFromText(objectTypeText);
            record.avRecordSignature = manifestRecord.recordSignature;
            record.threatName = threatName.empty() ? L"unknown" : threatName;
            if (record.objectType == AvEngine::ObjectType::Unknown)
            {
                return false;
            }
            return true;
        }

        std::vector<unsigned char> BuildRecordCanonicalPayload(const AvEngine::AvRecord& record, const std::wstring& objectTypeText)
        {
            std::vector<unsigned char> prefixBytes(kPrefixLength);
            uint64_t prefix = record.objectSignaturePrefix;
            for (size_t index = 0; index < kPrefixLength; ++index)
            {
                prefixBytes[kPrefixLength - index - 1] = static_cast<unsigned char>(prefix & 0xFF);
                prefix >>= 8;
            }

            const std::wstring json =
                L"{\"objectSignature\":\"" + BytesToHexWide(record.objectSignature) +
                L"\",\"objectSignatureLength\":" + std::to_wstring(record.objectSignatureLength) +
                L",\"objectSignaturePrefix\":\"" + BytesToHexWide(prefixBytes) +
                L"\",\"objectType\":\"" + JsonEscape(objectTypeText) +
                L"\",\"offsetBegin\":" + std::to_wstring(record.offsetBegin) +
                L",\"offsetEnd\":" + std::to_wstring(record.offsetEnd) + L"}";
            const std::string utf8 = WideToUtf8(json);
            return std::vector<unsigned char>(utf8.begin(), utf8.end());
        }

        bool ParseManifest(const std::vector<unsigned char>& manifestBytes,
            std::vector<ManifestRecord>& records,
            std::vector<unsigned char>& dataHash,
            std::vector<unsigned char>& unsignedManifest,
            std::vector<unsigned char>& manifestSignature,
            std::wstring& releaseDate,
            std::wstring& errorMessage)
        {
            records.clear();
            dataHash.clear();
            unsignedManifest.clear();
            manifestSignature.clear();
            releaseDate.clear();

            DataReader reader{ manifestBytes };
            if (!reader.CanRead(4) ||
                manifestBytes[0] != kManifestMagic[0] ||
                manifestBytes[1] != kManifestMagic[1] ||
                manifestBytes[2] != kManifestMagic[2] ||
                manifestBytes[3] != kManifestMagic[3])
            {
                errorMessage = L"Manifest magic is invalid.";
                return false;
            }
            reader.pos = 4;

            uint16_t version = 0;
            uint8_t exportType = 0;
            int64_t generatedAt = 0;
            int64_t since = 0;
            uint32_t recordCount = 0;
            if (!reader.ReadU16(version) || version != kFormatVersion ||
                !reader.ReadU8(exportType) ||
                !reader.ReadI64(generatedAt) ||
                !reader.ReadI64(since) ||
                !reader.ReadU32(recordCount))
            {
                errorMessage = L"Manifest header is invalid.";
                return false;
            }

            if (!reader.ReadBytes(kSha256Length, dataHash))
            {
                errorMessage = L"Manifest data hash is missing.";
                return false;
            }

            records.reserve(recordCount);
            for (uint32_t index = 0; index < recordCount; ++index)
            {
                std::vector<unsigned char> uuidBytes;
                uint32_t recordSignatureLength = 0;
                ManifestRecord item;
                if (!reader.ReadBytes(16, uuidBytes) ||
                    !reader.ReadU8(item.status) ||
                    !reader.ReadI64(item.updatedAt) ||
                    !reader.ReadU64(item.dataOffset) ||
                    !reader.ReadU32(item.dataLength) ||
                    !reader.ReadU32(recordSignatureLength) ||
                    recordSignatureLength > 64 * 1024 ||
                    !reader.ReadBytes(recordSignatureLength, item.recordSignature))
                {
                    errorMessage = L"Manifest record is invalid.";
                    return false;
                }
                item.id = GuidFromBytes(uuidBytes);
                records.push_back(std::move(item));
            }

            const size_t unsignedLength = reader.pos;
            uint32_t manifestSignatureLength = 0;
            if (!reader.ReadU32(manifestSignatureLength) ||
                manifestSignatureLength > 64 * 1024 ||
                !reader.ReadBytes(manifestSignatureLength, manifestSignature) ||
                reader.pos != manifestBytes.size())
            {
                errorMessage = L"Manifest signature block is invalid.";
                return false;
            }

            unsignedManifest.assign(manifestBytes.begin(), manifestBytes.begin() + static_cast<std::ptrdiff_t>(unsignedLength));
            releaseDate = FormatReleaseDate(generatedAt);
            return true;
        }

        bool ValidateDataHeader(const std::vector<unsigned char>& dataBytes, uint32_t expectedCount, std::wstring& errorMessage)
        {
            DataReader reader{ dataBytes };
            if (!reader.CanRead(kDataHeaderSize) ||
                dataBytes[0] != kDataMagic[0] ||
                dataBytes[1] != kDataMagic[1] ||
                dataBytes[2] != kDataMagic[2] ||
                dataBytes[3] != kDataMagic[3])
            {
                errorMessage = L"Data file magic is invalid.";
                return false;
            }
            reader.pos = 4;
            uint16_t version = 0;
            uint32_t count = 0;
            if (!reader.ReadU16(version) || version != kFormatVersion || !reader.ReadU32(count))
            {
                errorMessage = L"Data file header is invalid.";
                return false;
            }
            if (count != expectedCount)
            {
                errorMessage = L"Data file record count does not match manifest.";
                return false;
            }
            return true;
        }

        bool LoadFilesFromDirectory(const std::wstring& directory, BinaryPackage& package)
        {
            return ReadFileBytes(JoinPath(directory, L"manifest.bin"), package.manifest) &&
                ReadFileBytes(JoinPath(directory, L"data.bin"), package.data);
        }
    }

    std::wstring GetProgramDataDatabaseDirectory()
    {
        return JoinPath(ExpandPath(L"%ProgramData%"), L"ZIoVPO\\AvDb");
    }

    std::wstring GetProgramDataBackupDirectory()
    {
        return JoinPath(ExpandPath(L"%ProgramData%"), L"ZIoVPO\\AvDbBackup");
    }

    std::wstring GetExecutableDirectory()
    {
        wchar_t modulePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, modulePath, ARRAYSIZE(modulePath));
        std::filesystem::path path(modulePath);
        return path.parent_path().wstring();
    }

    std::wstring GetDefaultDatabaseDirectory()
    {
        return JoinPath(GetExecutableDirectory(), L"DefaultAvDb");
    }

    std::wstring GetPublicCertificatePath()
    {
        return JoinPath(GetExecutableDirectory(), L"signature_public.pem");
    }

    bool SavePackageToDirectory(const std::wstring& directory, const BinaryPackage& package, std::wstring& errorMessage)
    {
        errorMessage.clear();
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        if (ec)
        {
            errorMessage = L"Cannot create antivirus database directory.";
            return false;
        }
        if (!WriteFileBytes(JoinPath(directory, L"manifest.bin"), package.manifest) ||
            !WriteFileBytes(JoinPath(directory, L"data.bin"), package.data))
        {
            errorMessage = L"Cannot write antivirus database files.";
            return false;
        }
        return true;
    }

    bool CopyDirectoryFiles(const std::wstring& sourceDirectory, const std::wstring& targetDirectory, std::wstring& errorMessage)
    {
        errorMessage.clear();
        BinaryPackage package;
        if (!LoadFilesFromDirectory(sourceDirectory, package))
        {
            errorMessage = L"Source antivirus database files are missing.";
            return false;
        }
        return SavePackageToDirectory(targetDirectory, package, errorMessage);
    }

    bool LoadFromDirectory(const std::wstring& directory, const std::wstring& certificatePath, LoadResult& result)
    {
        result = {};
        BinaryPackage package;
        if (!LoadFilesFromDirectory(directory, package))
        {
            result.errorMessage = L"Antivirus database files are missing.";
            return false;
        }
        return LoadFromPackage(package, certificatePath, result);
    }

    bool LoadFromPackage(const BinaryPackage& package, const std::wstring& certificatePath, LoadResult& result)
    {
        result = {};
        std::vector<ManifestRecord> manifestRecords;
        std::vector<unsigned char> expectedDataHash;
        std::vector<unsigned char> unsignedManifest;
        std::vector<unsigned char> manifestSignature;
        std::wstring releaseDate;

        if (!ParseManifest(package.manifest, manifestRecords, expectedDataHash, unsignedManifest, manifestSignature, releaseDate, result.errorMessage))
        {
            return false;
        }

        std::wstring verifyError;
        if (!SignatureVerifier::VerifySha256RsaSignature(unsignedManifest, manifestSignature, certificatePath, verifyError))
        {
            result.manifestSignatureFailed = true;
            result.errorMessage = L"Manifest signature verification failed: " + verifyError;
            return false;
        }

        if (!ValidateDataHeader(package.data, static_cast<uint32_t>(manifestRecords.size()), result.errorMessage))
        {
            return false;
        }

        const std::vector<unsigned char> actualDataHash = Sha256(package.data);
        if (actualDataHash.empty() || actualDataHash != expectedDataHash)
        {
            result.dataHashFailed = true;
            result.errorMessage = L"Data file SHA-256 check failed.";
            return false;
        }

        std::vector<AvEngine::AvRecord> verifiedRecords;
        verifiedRecords.reserve(manifestRecords.size());
        for (const ManifestRecord& manifestRecord : manifestRecords)
        {
            if (manifestRecord.status != 1)
            {
                continue;
            }

            AvEngine::AvRecord record;
            if (!ParseDataRecord(package.data, manifestRecord, record))
            {
                result.invalidRecordIds.push_back(manifestRecord.id);
                continue;
            }

            std::wstring objectTypeText;
            // The exact text value is needed for record signature verification, therefore it is read again from the data record.
            const uint64_t absoluteOffset = static_cast<uint64_t>(kDataHeaderSize) + manifestRecord.dataOffset;
            std::vector<unsigned char> recordBytes(
                package.data.begin() + static_cast<std::ptrdiff_t>(absoluteOffset),
                package.data.begin() + static_cast<std::ptrdiff_t>(absoluteOffset + manifestRecord.dataLength));
            DataReader recordReader{ recordBytes };
            std::wstring threatName;
            std::vector<unsigned char> prefixBytes;
            std::vector<unsigned char> remainderBytes;
            int64_t remainderLength = 0;
            int64_t offsetStart = 0;
            int64_t offsetEnd = 0;
            ReadUtf8(recordReader, threatName);
            ReadLengthPrefixedBytes(recordReader, prefixBytes);
            ReadLengthPrefixedBytes(recordReader, remainderBytes);
            recordReader.ReadI64(remainderLength);
            ReadUtf8(recordReader, objectTypeText);
            recordReader.ReadI64(offsetStart);
            recordReader.ReadI64(offsetEnd);

            const std::vector<unsigned char> recordPayload = BuildRecordCanonicalPayload(record, objectTypeText);
            std::wstring recordVerifyError;
            if (!SignatureVerifier::VerifySha256RsaSignature(recordPayload, manifestRecord.recordSignature, certificatePath, recordVerifyError))
            {
                result.invalidRecordIds.push_back(manifestRecord.id);
                continue;
            }

            verifiedRecords.push_back(std::move(record));
        }

        if (!AvEngine::BuildDatabaseFromRecords(verifiedRecords, releaseDate, result.database, result.errorMessage))
        {
            return false;
        }

        result.success = true;
        result.usedOnlyVerifiedRecords = true;
        return true;
    }
}
