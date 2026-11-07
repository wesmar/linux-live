#include "support/Common.h"
#include "support/Text.h"
#include <Windows.h>
#include <format>
#include <memory>

namespace DL {

std::wstring ErrorText(std::wstring_view what, unsigned long code) {
    struct LocalFreer {
        void operator()(wchar_t* p) const noexcept { LocalFree(p); }
    };
    auto load = [code](DWORD flags, HMODULE source, DWORD language) {
        wchar_t* buffer = nullptr;
        const DWORD length = FormatMessageW(flags | FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS, source, code,
                                            language, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
        const std::unique_ptr<wchar_t, LocalFreer> owner(buffer);
        return length && buffer ? std::wstring(buffer, length) : std::wstring();
    };
    // In the language of the interface when Windows has it, otherwise the system default.
    const DWORD language = Text::SystemLanguage();
    std::wstring message = load(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, language);
    if (message.empty() && language) message = load(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, 0);
    if (message.empty()) {
        // WMI (WBEM_E_*) error codes are not in the system message table; they live in wmiutils.dll.
        static const HMODULE wmi = LoadLibraryExW(L"wmiutils.dll", nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (wmi) message = load(FORMAT_MESSAGE_FROM_HMODULE, wmi, 0);
    }
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) message.pop_back();
    if (message.empty()) message = Text::Get(IDS_UNKNOWN_ERROR);
    // HRESULTs read better in hex.
    const std::wstring shown = code >= 0x80000000ul ? std::format(L"0x{:08X}", code) : std::format(L"{}", code);
    return Text::Format(IDS_ERROR_FORMAT, what, message, shown);
}

std::wstring Capitalize(std::wstring_view text) {
    std::wstring out(text);
    if (!out.empty()) CharUpperBuffW(out.data(), 1);
    return out;
}

std::wstring FormatBytes(std::uint64_t bytes) {
    constexpr double kMiB = 1024.0 * 1024.0;
    if (bytes >= (1ull << 30)) return std::format(L"{:.2f} GB", static_cast<double>(bytes) / (kMiB * 1024.0));
    if (bytes >= (1ull << 20)) return std::format(L"{:.1f} MB", static_cast<double>(bytes) / kMiB);
    return std::format(L"{:.1f} KB", static_cast<double>(bytes) / 1024.0);
}

std::wstring FormatDuration(std::uint64_t seconds) {
    const std::uint64_t h = seconds / 3600, m = seconds / 60 % 60, s = seconds % 60;
    return h ? std::format(L"{}:{:02}:{:02}", h, m, s) : std::format(L"{}:{:02}", m, s);
}

std::string ToUtf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof elevation, &size) && elevation.TokenIsElevated;
    CloseHandle(token);
    return ok;
}

}  // namespace DL
