// Incremental SHA-256 (Windows CNG / bcrypt).
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace DL {

class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    bool ok() const noexcept { return m_hash != nullptr; }
    bool Update(const void* data, std::size_t size);
    std::wstring Finish();  // lowercase hex; empty on failure. Call once.

private:
    void* m_alg = nullptr;
    void* m_hash = nullptr;
    std::vector<unsigned char> m_object;
};

}  // namespace DL
