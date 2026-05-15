#pragma once

#include "AvEngine.h"

#include <string>
#include <vector>

namespace AvDatabaseStorage
{
    struct BinaryPackage
    {
        std::vector<unsigned char> manifest;
        std::vector<unsigned char> data;
    };

    struct LoadResult
    {
        bool success = false;
        bool manifestSignatureFailed = false;
        bool dataHashFailed = false;
        bool usedOnlyVerifiedRecords = false;
        AvEngine::AvDatabase database;
        std::vector<std::wstring> invalidRecordIds;
        std::wstring errorMessage;
    };

    std::wstring GetProgramDataDatabaseDirectory();
    std::wstring GetProgramDataBackupDirectory();
    std::wstring GetExecutableDirectory();
    std::wstring GetDefaultDatabaseDirectory();
    std::wstring GetPublicCertificatePath();

    bool SavePackageToDirectory(const std::wstring& directory, const BinaryPackage& package, std::wstring& errorMessage);
    bool CopyDirectoryFiles(const std::wstring& sourceDirectory, const std::wstring& targetDirectory, std::wstring& errorMessage);
    bool LoadFromDirectory(const std::wstring& directory, const std::wstring& certificatePath, LoadResult& result);
    bool LoadFromPackage(const BinaryPackage& package, const std::wstring& certificatePath, LoadResult& result);
}
