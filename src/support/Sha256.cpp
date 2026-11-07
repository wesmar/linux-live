#include "support/Sha256.h"
#include <Windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

namespace DL {

Sha256::Sha256() {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) return;
    m_alg = alg;
    DWORD objectLength = 0, got = 0;
    if (!BCRYPT_SUCCESS(BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength),
                                          sizeof objectLength, &got, 0)))
        return;
    m_object.resize(objectLength);
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, m_object.data(), objectLength, nullptr, 0, 0))) return;
    m_hash = hash;
}

Sha256::~Sha256() {
    if (m_hash) BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(m_hash));
    if (m_alg) BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(m_alg), 0);
}

bool Sha256::Update(const void* data, std::size_t size) {
    if (!m_hash) return false;
    return BCRYPT_SUCCESS(BCryptHashData(static_cast<BCRYPT_HASH_HANDLE>(m_hash),
                                         static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(size), 0));
}

std::wstring Sha256::Finish() {
    if (!m_hash) return {};
    unsigned char digest[32]{};
    if (!BCRYPT_SUCCESS(BCryptFinishHash(static_cast<BCRYPT_HASH_HANDLE>(m_hash), digest, sizeof digest, 0))) return {};
    constexpr wchar_t kHex[] = L"0123456789abcdef";
    std::wstring out;
    out.reserve(64);
    for (unsigned char b : digest) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0xF]);
    }
    return out;
}

}  // namespace DL
