#include "support/Settings.h"
#include "support/Config.h"
#include <vector>

namespace DL::Settings {

std::wstring Read(const wchar_t* name, std::wstring_view fallback) {
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, Config::kRegKey, name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes < sizeof(wchar_t))
        return std::wstring(fallback);
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    bytes = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_CURRENT_USER, Config::kRegKey, name, RRF_RT_REG_SZ, nullptr, buffer.data(), &bytes) != ERROR_SUCCESS)
        return std::wstring(fallback);
    return buffer.data();
}

void Write(const wchar_t* name, const std::wstring& value) {
    RegSetKeyValueW(HKEY_CURRENT_USER, Config::kRegKey, name, REG_SZ, value.c_str(),
                    static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

DWORD ReadDword(const wchar_t* name, DWORD fallback) {
    DWORD value = 0, bytes = sizeof value;
    if (RegGetValueW(HKEY_CURRENT_USER, Config::kRegKey, name, RRF_RT_REG_DWORD, nullptr, &value, &bytes) != ERROR_SUCCESS)
        return fallback;
    return value;
}

void WriteDword(const wchar_t* name, DWORD value) {
    RegSetKeyValueW(HKEY_CURRENT_USER, Config::kRegKey, name, REG_DWORD, &value, sizeof value);
}

bool Exists(const wchar_t* name) {
    return RegGetValueW(HKEY_CURRENT_USER, Config::kRegKey, name, RRF_RT_ANY, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

}  // namespace DL::Settings
