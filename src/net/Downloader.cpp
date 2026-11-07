#include "net/Downloader.h"
#include "support/Sha256.h"
#include "support/Text.h"
#include <Windows.h>
#include <winhttp.h>
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <format>
#include <fstream>
#include <memory>
#include <type_traits>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace DL::Net {
namespace {

struct InternetCloser {
    void operator()(HINTERNET h) const noexcept { if (h) WinHttpCloseHandle(h); }
};
using Internet = std::unique_ptr<std::remove_pointer_t<HINTERNET>, InternetCloser>;

// One sent request with its response headers received.
struct Http {
    Internet session, connection, request;
    DWORD status = 0;
};

Result<Http> Open(const std::wstring& url, const wchar_t* verb, DWORD connectMs, DWORD ioMs) {
    wchar_t host[256]{}, path[2048]{}, extra[2048]{};
    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host;     uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;      uc.dwUrlPathLength = 2048;
    uc.lpszExtraInfo = extra;   uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return Fail(ErrorText(Text::Get(IDS_CTX_INVALID_URL), GetLastError()));
    if (uc.nScheme != INTERNET_SCHEME_HTTPS) return Fail(Text::Get(IDS_HTTPS_ONLY));
    const std::wstring target = std::wstring(path) + extra;

    Http http;
    http.session.reset(WinHttpOpen(L"LinuxLiveInstaller/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!http.session) return Fail(ErrorText(L"WinHttpOpen", GetLastError()));
    WinHttpSetTimeouts(http.session.get(), static_cast<int>(connectMs), static_cast<int>(connectMs),
                       static_cast<int>(ioMs), static_cast<int>(ioMs));

    http.connection.reset(WinHttpConnect(http.session.get(), host, uc.nPort, 0));
    if (!http.connection) return Fail(ErrorText(Text::Get(IDS_CTX_CONNECT), GetLastError()));

    http.request.reset(WinHttpOpenRequest(http.connection.get(), verb, target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!http.request) return Fail(ErrorText(L"WinHttpOpenRequest", GetLastError()));

    // Follow redirects, but never from https to http.
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(http.request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    if (!WinHttpSendRequest(http.request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(http.request.get(), nullptr))
        return Fail(ErrorText(Text::Get(IDS_CTX_REQUEST), GetLastError()));

    DWORD size = sizeof http.status;
    if (!WinHttpQueryHeaders(http.request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &http.status, &size, WINHTTP_NO_HEADER_INDEX))
        return Fail(ErrorText(Text::Get(IDS_CTX_STATUS), GetLastError()));
    return http;
}

std::uint64_t ContentLength(HINTERNET request) {
    ULONGLONG total = 0;
    DWORD size = sizeof total;
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER64,
                             WINHTTP_HEADER_NAME_BY_INDEX, &total, &size, WINHTTP_NO_HEADER_INDEX))
        return 0;
    return total;
}

std::wstring HeaderText(HINTERNET request, DWORD info) {
    DWORD bytes = 0;
    SetLastError(0);
    WinHttpQueryHeaders(request, info, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &bytes,
                        WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes < sizeof(wchar_t)) return {};
    std::wstring text(bytes / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, info, WINHTTP_HEADER_NAME_BY_INDEX, text.data(), &bytes, WINHTTP_NO_HEADER_INDEX))
        return {};
    text.resize(bytes / sizeof(wchar_t));
    return text;
}

Result<> DownloadTo(const std::wstring& url, const std::filesystem::path& part, const ProgressFn& progress,
                    std::stop_token stop, std::wstring_view expectedSha256) {
    auto http = Open(url, L"GET", 15000, 30000);
    if (!http) return Fail(http.error());
    if (http->status != 200) return Fail(Text::Format(IDS_HTTP_STATUS, http->status));
    HINTERNET request = http->request.get();
    const ULONGLONG total = ContentLength(request);

    Sha256 hash;
    const bool verify = !expectedSha256.empty();
    if (verify && !hash.ok()) return Fail(Text::Get(IDS_SHA_UNAVAILABLE));

    std::error_code ec;
    std::filesystem::create_directories(part.parent_path(), ec);
    std::ofstream file(part, std::ios::binary | std::ios::trunc);
    if (!file) return Fail(Text::Format(IDS_CANNOT_CREATE_FILE, part.wstring()));

    std::vector<char> buffer(256 * 1024);
    ULONGLONG done = 0;
    progress(0, total);
    for (;;) {
        if (stop.stop_requested()) return Fail(Text::Get(IDS_CANCELED));
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) return Fail(ErrorText(Text::Get(IDS_CTX_DOWNLOADING), GetLastError()));
        if (!available) break;
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &read))
            return Fail(ErrorText(Text::Get(IDS_CTX_DOWNLOADING), GetLastError()));
        if (!read) break;
        file.write(buffer.data(), read);
        if (!file) return Fail(Text::Get(IDS_DISK_WRITE));
        if (verify) hash.Update(buffer.data(), read);
        done += read;
        progress(done, total);
    }
    file.close();
    if (!file) return Fail(Text::Get(IDS_FILE_CLOSE));
    if (total && done != total) return Fail(Text::Get(IDS_INCOMPLETE));
    if (verify) {
        const std::wstring actual = hash.Finish();
        if (actual != expectedSha256)
            return Fail(Text::Format(IDS_SHA_MISMATCH,
                                    expectedSha256, actual));
    }
    progress(done, total);
    return {};
}

bool IsHex64(const std::wstring& s) {
    return s.size() == 64 && std::ranges::all_of(s, [](wchar_t c) { return std::iswxdigit(c) != 0; });
}

}  // namespace

std::wstring RemoteFileInfo::Stamp() const { return std::format(L"{}|{}|{}", size, etag, lastModified); }

Result<RemoteFileInfo> CheckRemote(const std::wstring& url) {
    auto http = Open(url, L"HEAD", 10000, 15000);
    if (!http) return Fail(http.error());
    if (http->status != 200) return Fail(Text::Format(IDS_HTTP_STATUS, http->status));

    RemoteFileInfo info;
    info.available = true;
    info.size = ContentLength(http->request.get());
    info.etag = HeaderText(http->request.get(), WINHTTP_QUERY_ETAG);
    info.lastModified = HeaderText(http->request.get(), WINHTTP_QUERY_LAST_MODIFIED);
    return info;
}

Result<std::optional<std::wstring>> FetchSha256(const std::wstring& url) {
    std::wstring sidecar = url;
    if (const auto q = sidecar.find(L'?'); q == std::wstring::npos) sidecar += L".sha256";
    else sidecar.insert(q, L".sha256");

    auto http = Open(sidecar, L"GET", 10000, 15000);
    if (!http) return Fail(http.error());
    if (http->status == 404 || http->status == 410) return std::optional<std::wstring>{};
    if (http->status != 200) return Fail(Text::Format(IDS_CHECKSUM_STATUS, http->status));

    std::string body;
    char chunk[512];
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(http->request.get(), &available)) return Fail(ErrorText(Text::Get(IDS_CTX_CHECKSUM), GetLastError()));
        if (!available) break;
        DWORD read = 0;
        if (!WinHttpReadData(http->request.get(), chunk, std::min<DWORD>(available, static_cast<DWORD>(sizeof chunk)), &read))
            return Fail(ErrorText(Text::Get(IDS_CTX_CHECKSUM), GetLastError()));
        if (!read) break;
        body.append(chunk, read);
        if (body.size() > 4096) return Fail(Text::Get(IDS_CHECKSUM_TOO_LARGE));
    }

    // First whitespace-delimited token must be the 64-digit digest.
    std::size_t begin = 0;
    while (begin < body.size() && std::isspace(static_cast<unsigned char>(body[begin]))) ++begin;
    std::size_t end = begin;
    while (end < body.size() && !std::isspace(static_cast<unsigned char>(body[end]))) ++end;
    std::wstring digest;
    for (std::size_t i = begin; i < end; ++i)
        digest.push_back(static_cast<wchar_t>(std::tolower(static_cast<unsigned char>(body[i]))));
    if (!IsHex64(digest)) return Fail(Text::Get(IDS_CHECKSUM_FORMAT));
    return std::optional<std::wstring>{std::move(digest)};
}

Result<> Download(const std::wstring& url, const std::filesystem::path& destination, const ProgressFn& progress,
                  std::stop_token stop, std::wstring_view expectedSha256) {
    auto part = destination;
    part += L".part";
    auto result = DownloadTo(url, part, progress, stop, expectedSha256);
    std::error_code ec;
    if (!result) {
        std::filesystem::remove(part, ec);
        return result;
    }
    if (!MoveFileExW(part.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD e = GetLastError();
        std::filesystem::remove(part, ec);
        return Fail(ErrorText(Text::Format(IDS_CTX_SAVE, destination.wstring()), e));
    }
    return {};
}

}  // namespace DL::Net
