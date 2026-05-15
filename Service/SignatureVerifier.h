#pragma once

#include <string>
#include <vector>

namespace SignatureVerifier
{
    bool VerifySha256RsaSignature(
        const std::vector<unsigned char>& payload,
        const std::vector<unsigned char>& signature,
        const std::wstring& certificatePath,
        std::wstring& errorMessage);
}
