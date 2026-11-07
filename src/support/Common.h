// Shared helpers: Result type and error formatting.
#pragma once
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace DL {

template <class T = void>
using Result = std::expected<T, std::wstring>;

inline std::unexpected<std::wstring> Fail(std::wstring message) {
    return std::unexpected<std::wstring>(std::move(message));
}

// "what: <system message> (code N)"
std::wstring ErrorText(std::wstring_view what, unsigned long code);

// "123.4 MB" style size formatting.
std::wstring FormatBytes(std::uint64_t bytes);

// First character upper-cased ("image" -> "Image"), for names at the start of a sentence.
std::wstring Capitalize(std::wstring_view text);

// "1:05" / "1:02:03" style duration.
std::wstring FormatDuration(std::uint64_t seconds);

// UTF-16 <-> UTF-8.
std::string ToUtf8(std::wstring_view text);

// True when the process token is elevated (administrator).
bool IsElevated();

// Severity of a log line (GUI colours, CLI tags).
enum class Level { Info, Success, Warning, Error };

// Progress callback: bytes done, bytes total (0 = unknown).
using ProgressFn = std::function<void(std::uint64_t done, std::uint64_t total)>;
using LogFn = std::function<void(std::wstring)>;
// Yes/no question to the person (answered on the UI thread); true = yes.
using ConfirmFn = std::function<bool(const std::wstring&)>;

}  // namespace DL
