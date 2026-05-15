#define NOMINMAX

#include "SignatureVerifier.h"

#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Crypt32.lib")

namespace SignatureVerifier
{
    namespace
    {
        bool ReadTextFile(const std::wstring& path, std::string& text)
        {
            std::ifstream file(std::filesystem::path(path), std::ios::binary);
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

            text.resize(static_cast<size_t>(length));
            if (!text.empty())
            {
                file.read(text.data(), length);
            }
            return file.good() || file.eof();
        }

        PCCERT_CONTEXT LoadCertificate(const std::wstring& certificatePath, std::wstring& errorMessage)
        {
            std::string pem;
            if (!ReadTextFile(certificatePath, pem))
            {
                errorMessage = L"Public certificate file is not available.";
                return nullptr;
            }

            DWORD derSize = 0;
            if (!CryptStringToBinaryA(
                pem.c_str(),
                static_cast<DWORD>(pem.size()),
                CRYPT_STRING_BASE64HEADER,
                nullptr,
                &derSize,
                nullptr,
                nullptr) || derSize == 0)
            {
                errorMessage = L"Public certificate has invalid PEM format.";
                return nullptr;
            }

            std::vector<unsigned char> der(derSize);
            if (!CryptStringToBinaryA(
                pem.c_str(),
                static_cast<DWORD>(pem.size()),
                CRYPT_STRING_BASE64HEADER,
                der.data(),
                &derSize,
                nullptr,
                nullptr))
            {
                errorMessage = L"Public certificate cannot be decoded.";
                return nullptr;
            }
            der.resize(derSize);

            PCCERT_CONTEXT certContext = CertCreateCertificateContext(
                X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                der.data(),
                static_cast<DWORD>(der.size()));
            if (certContext == nullptr)
            {
                errorMessage = L"Public certificate cannot be opened.";
            }
            return certContext;
        }
    }

    bool VerifySha256RsaSignature(
        const std::vector<unsigned char>& payload,
        const std::vector<unsigned char>& signature,
        const std::wstring& certificatePath,
        std::wstring& errorMessage)
    {
        errorMessage.clear();
        if (payload.empty() || signature.empty())
        {
            errorMessage = L"Signature verification input is empty.";
            return false;
        }

        PCCERT_CONTEXT certContext = LoadCertificate(certificatePath, errorMessage);
        if (certContext == nullptr)
        {
            return false;
        }

        HCRYPTPROV provider = 0;
        HCRYPTKEY publicKey = 0;
        HCRYPTHASH hash = 0;
        bool ok = false;

        if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        {
            errorMessage = L"Cannot create cryptographic provider.";
            CertFreeCertificateContext(certContext);
            return false;
        }

        if (!CryptImportPublicKeyInfo(
            provider,
            X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
            &certContext->pCertInfo->SubjectPublicKeyInfo,
            &publicKey))
        {
            errorMessage = L"Cannot import public key from certificate.";
            CryptReleaseContext(provider, 0);
            CertFreeCertificateContext(certContext);
            return false;
        }

        if (!CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash))
        {
            errorMessage = L"Cannot create SHA-256 hash.";
        }
        else if (!CryptHashData(hash, payload.data(), static_cast<DWORD>(payload.size()), 0))
        {
            errorMessage = L"Cannot hash signed payload.";
        }
        else
        {
            std::vector<unsigned char> cryptoApiSignature = signature;
            std::reverse(cryptoApiSignature.begin(), cryptoApiSignature.end());
            ok = CryptVerifySignatureW(
                hash,
                cryptoApiSignature.data(),
                static_cast<DWORD>(cryptoApiSignature.size()),
                publicKey,
                nullptr,
                0) == TRUE;
            if (!ok)
            {
                errorMessage = L"RSA signature verification failed.";
            }
        }

        if (hash != 0)
        {
            CryptDestroyHash(hash);
        }
        if (publicKey != 0)
        {
            CryptDestroyKey(publicKey);
        }
        CryptReleaseContext(provider, 0);
        CertFreeCertificateContext(certContext);
        return ok;
    }
}
