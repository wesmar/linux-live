// Minimal WMI client over COM (no comdef.h / comsupp): connect to a namespace, query, call methods.
// Used for BitLocker (root\CIMV2\Security\MicrosoftVolumeEncryption), the Windows Storage
// Management API (root\Microsoft\Windows\Storage) and the BCD store (root\WMI).
#pragma once
#include "support/Common.h"
#include <Windows.h>
#include <wbemidl.h>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace DL::Wmi {

// Owning COM interface pointer (move-only).
template <class T>
class Ptr {
public:
    Ptr() = default;
    Ptr(const Ptr&) = delete;
    Ptr& operator=(const Ptr&) = delete;
    Ptr(Ptr&& other) noexcept : m_p(std::exchange(other.m_p, nullptr)) {}
    Ptr& operator=(Ptr&& other) noexcept {
        if (this != &other) {
            reset();
            m_p = std::exchange(other.m_p, nullptr);
        }
        return *this;
    }
    ~Ptr() { reset(); }

    // Takes over a reference the caller already owns.
    static Ptr Attach(T* p) {
        Ptr result;
        result.m_p = p;
        return result;
    }
    T** put() {
        reset();
        return &m_p;
    }
    T* get() const { return m_p; }
    T* operator->() const { return m_p; }
    explicit operator bool() const { return m_p != nullptr; }
    void reset() {
        if (m_p) {
            m_p->Release();
            m_p = nullptr;
        }
    }

private:
    T* m_p = nullptr;
};

class Bstr {
public:
    explicit Bstr(std::wstring_view text) : m_s(SysAllocStringLen(text.data(), static_cast<UINT>(text.size()))) {}
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    ~Bstr() { SysFreeString(m_s); }
    operator BSTR() const { return m_s; }

private:
    BSTR m_s;
};

struct Variant {
    VARIANT v;
    Variant() { VariantInit(&v); }
    Variant(const Variant&) = delete;
    Variant& operator=(const Variant&) = delete;
    ~Variant() { VariantClear(&v); }
};

// COM for the calling thread. A thread that already runs COM (the GUI thread) keeps its model.
class ComScope {
public:
    ComScope() : m_uninit(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    ~ComScope() {
        if (m_uninit) CoUninitialize();
    }

private:
    bool m_uninit;
};

using Object = Ptr<IWbemClassObject>;

// --- Properties --------------------------------------------------------------------------------
// WMI hands uint64 values over as decimal strings; GetU64 / PutU64 convert.
std::optional<std::uint32_t> GetU32(IWbemClassObject* obj, const wchar_t* name);
std::optional<std::uint64_t> GetU64(IWbemClassObject* obj, const wchar_t* name);
std::optional<bool> GetBool(IWbemClassObject* obj, const wchar_t* name);
std::wstring GetString(IWbemClassObject* obj, const wchar_t* name);
std::vector<std::wstring> GetStrings(IWbemClassObject* obj, const wchar_t* name);
Object GetEmbedded(IWbemClassObject* obj, const wchar_t* name);                   // embedded object
std::vector<Object> GetEmbeddedArray(IWbemClassObject* obj, const wchar_t* name);  // array of embedded objects

HRESULT PutU32(IWbemClassObject* obj, const wchar_t* name, std::uint32_t value);
HRESULT PutU64(IWbemClassObject* obj, const wchar_t* name, std::uint64_t value);
HRESULT PutBool(IWbemClassObject* obj, const wchar_t* name, bool value);
HRESULT PutString(IWbemClassObject* obj, const wchar_t* name, std::wstring_view value);

// ReturnValue of a method call; a missing ReturnValue counts as 0 (success).
std::uint32_t ReturnValue(IWbemClassObject* out);

// Text for a WQL string literal: backslashes and quotes escaped.
std::wstring Quote(std::wstring_view text);

// --- Connection --------------------------------------------------------------------------------
class Service {
public:
    // e.g. L"ROOT\\Microsoft\\Windows\\Storage". The caller keeps a ComScope alive meanwhile.
    static Result<Service> Connect(const wchar_t* ns);

    Result<std::vector<Object>> Query(std::wstring_view wql) const;
    // Empty in-parameters of `className`.`method`, to be filled with Put*; null when it takes none.
    Result<Object> InParams(const wchar_t* className, const wchar_t* method) const;
    // Calls `method` on an object path (an instance __PATH / __RELPATH, or a class name for static
    // methods). HRESULT failures become errors; ReturnValue is left to the caller.
    Result<Object> Call(std::wstring_view objectPath, const wchar_t* method, IWbemClassObject* in) const;

private:
    Ptr<IWbemServices> m_svc;
};

}  // namespace DL::Wmi
