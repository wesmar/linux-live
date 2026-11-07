#include "support/Wmi.h"
#include <cwchar>

#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace DL::Wmi {
namespace {

void Blanket(IUnknown* proxy) {
    CoSetProxyBlanket(proxy, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                      nullptr, EOAC_NONE);
}

std::wstring Hr(std::wstring_view what, HRESULT hr) { return ErrorText(what, static_cast<unsigned long>(hr)); }

template <class F>
void ForEachElement(SAFEARRAY* array, F&& f) {
    if (!array) return;
    LONG lower = 0, upper = -1;
    SafeArrayGetLBound(array, 1, &lower);
    SafeArrayGetUBound(array, 1, &upper);
    for (LONG i = lower; i <= upper; ++i) f(i);
}

}  // namespace

std::optional<std::uint32_t> GetU32(IWbemClassObject* obj, const wchar_t* name) {
    Variant var;
    if (!obj || FAILED(obj->Get(name, 0, &var.v, nullptr, nullptr))) return std::nullopt;
    switch (var.v.vt) {
    case VT_I4: return static_cast<std::uint32_t>(var.v.lVal);
    case VT_UI4: return var.v.ulVal;
    case VT_I2: return static_cast<std::uint32_t>(static_cast<std::uint16_t>(var.v.iVal));
    case VT_UI2: return var.v.uiVal;
    case VT_UI1: return var.v.bVal;
    default: return std::nullopt;
    }
}

std::optional<std::uint64_t> GetU64(IWbemClassObject* obj, const wchar_t* name) {
    Variant var;
    if (!obj || FAILED(obj->Get(name, 0, &var.v, nullptr, nullptr))) return std::nullopt;
    if (var.v.vt == VT_BSTR && var.v.bstrVal) {
        wchar_t* end = nullptr;
        const unsigned long long value = std::wcstoull(var.v.bstrVal, &end, 10);
        if (end == var.v.bstrVal) return std::nullopt;
        return value;
    }
    if (var.v.vt == VT_UI8) return var.v.ullVal;
    if (var.v.vt == VT_I8) return static_cast<std::uint64_t>(var.v.llVal);
    if (const auto narrow = GetU32(obj, name)) return *narrow;
    return std::nullopt;
}

std::optional<bool> GetBool(IWbemClassObject* obj, const wchar_t* name) {
    Variant var;
    if (!obj || FAILED(obj->Get(name, 0, &var.v, nullptr, nullptr)) || var.v.vt != VT_BOOL) return std::nullopt;
    return var.v.boolVal != VARIANT_FALSE;
}

std::wstring GetString(IWbemClassObject* obj, const wchar_t* name) {
    Variant var;
    if (!obj || FAILED(obj->Get(name, 0, &var.v, nullptr, nullptr)) || var.v.vt != VT_BSTR || !var.v.bstrVal) return {};
    return std::wstring(var.v.bstrVal, SysStringLen(var.v.bstrVal));
}

std::vector<std::wstring> GetStrings(IWbemClassObject* obj, const wchar_t* name) {
    std::vector<std::wstring> out;
    Variant var;
    if (!obj || FAILED(obj->Get(name, 0, &var.v, nullptr, nullptr)) || var.v.vt != (VT_ARRAY | VT_BSTR)) return out;
    ForEachElement(var.v.parray, [&](LONG i) {
        BSTR element = nullptr;
        if (SUCCEEDED(SafeArrayGetElement(var.v.parray, &i, &element)) && element) {
            out.emplace_back(element, SysStringLen(element));
            SysFreeString(element);
        }
    });
    return out;
}

Object GetEmbedded(IWbemClassObject* obj, const wchar_t* name) {
    Variant var;
    if (!obj || FAILED(obj->Get(name, 0, &var.v, nullptr, nullptr)) || var.v.vt != VT_UNKNOWN || !var.v.punkVal) return {};
    Object result;
    var.v.punkVal->QueryInterface(IID_IWbemClassObject, reinterpret_cast<void**>(result.put()));
    return result;
}

std::vector<Object> GetEmbeddedArray(IWbemClassObject* obj, const wchar_t* name) {
    std::vector<Object> out;
    Variant var;
    if (!obj || FAILED(obj->Get(name, 0, &var.v, nullptr, nullptr)) || var.v.vt != (VT_ARRAY | VT_UNKNOWN)) return out;
    ForEachElement(var.v.parray, [&](LONG i) {
        IUnknown* element = nullptr;
        if (SUCCEEDED(SafeArrayGetElement(var.v.parray, &i, &element)) && element) {
            Object item;
            if (SUCCEEDED(element->QueryInterface(IID_IWbemClassObject, reinterpret_cast<void**>(item.put())))) out.push_back(std::move(item));
            element->Release();
        }
    });
    return out;
}

HRESULT PutU32(IWbemClassObject* obj, const wchar_t* name, std::uint32_t value) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_I4;
    v.lVal = static_cast<LONG>(value);
    return obj->Put(name, 0, &v, 0);
}

HRESULT PutU64(IWbemClassObject* obj, const wchar_t* name, std::uint64_t value) {
    return PutString(obj, name, std::to_wstring(value));  // CIM uint64 travels as a decimal string
}

HRESULT PutBool(IWbemClassObject* obj, const wchar_t* name, bool value) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    return obj->Put(name, 0, &v, 0);
}

HRESULT PutString(IWbemClassObject* obj, const wchar_t* name, std::wstring_view value) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocStringLen(value.data(), static_cast<UINT>(value.size()));
    const HRESULT hr = obj->Put(name, 0, &v, 0);
    VariantClear(&v);
    return hr;
}

std::uint32_t ReturnValue(IWbemClassObject* out) { return GetU32(out, L"ReturnValue").value_or(0); }

std::wstring Quote(std::wstring_view text) {
    std::wstring out;
    out.reserve(text.size() + 8);
    for (const wchar_t c : text) {
        if (c == L'\\' || c == L'"' || c == L'\'') out += L'\\';
        out += c;
    }
    return out;
}

Result<Service> Service::Connect(const wchar_t* ns) {
    Ptr<IWbemLocator> locator;
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, reinterpret_cast<void**>(locator.put()));
    if (FAILED(hr)) return Fail(Hr(L"CoCreateInstance(WbemLocator)", hr));
    Service service;
    hr = locator->ConnectServer(Bstr(ns), nullptr, nullptr, nullptr, 0, nullptr, nullptr, service.m_svc.put());
    if (FAILED(hr)) return Fail(Hr(std::wstring(L"WMI ") + ns, hr));
    Blanket(service.m_svc.get());
    return service;
}

Result<std::vector<Object>> Service::Query(std::wstring_view wql) const {
    Ptr<IEnumWbemClassObject> items;
    const HRESULT hr = m_svc->ExecQuery(Bstr(L"WQL"), Bstr(wql), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, items.put());
    if (FAILED(hr)) return Fail(Hr(wql, hr));
    Blanket(items.get());
    std::vector<Object> out;
    for (;;) {
        Object object;
        ULONG returned = 0;
        if (items->Next(WBEM_INFINITE, 1, object.put(), &returned) != S_OK || returned == 0) break;
        out.push_back(std::move(object));
    }
    return out;
}

Result<Object> Service::InParams(const wchar_t* className, const wchar_t* method) const {
    Object cls;
    HRESULT hr = m_svc->GetObject(Bstr(className), 0, nullptr, cls.put(), nullptr);
    if (FAILED(hr)) return Fail(Hr(className, hr));
    Object definition;
    hr = cls->GetMethod(method, 0, definition.put(), nullptr);
    if (FAILED(hr)) return Fail(Hr(std::wstring(className) + L"." + method, hr));
    Object in;
    if (definition && FAILED(hr = definition->SpawnInstance(0, in.put()))) return Fail(Hr(method, hr));
    return in;
}

Result<Object> Service::Call(std::wstring_view objectPath, const wchar_t* method, IWbemClassObject* in) const {
    Object out;
    const HRESULT hr = m_svc->ExecMethod(Bstr(objectPath), Bstr(method), 0, nullptr, in, out.put(), nullptr);
    if (FAILED(hr)) return Fail(Hr(method, hr));
    return out;
}

}  // namespace DL::Wmi
