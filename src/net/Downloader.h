// HTTPS download and remote file inspection (WinHTTP). HTTPS only; https -> http redirects are refused.
#pragma once
#include "support/Common.h"
#include <filesystem>
#include <stop_token>

namespace DL::Net {

struct RemoteFileInfo {
    std::uint64_t size = 0;
    bool available = false;
    std::wstring etag;           // may be empty
    std::wstring lastModified;   // may be empty
    // "size|etag|last-modified": changes whenever the server-side file changes (when the server says so).
    std::wstring Stamp() const;
};

// Sends an HTTP HEAD request to check availability, size and version headers without downloading.
Result<RemoteFileInfo> CheckRemote(const std::wstring& url);

// Looks for "<url>.sha256" (a plain hex digest, optionally followed by a file name).
// Returns the lowercase digest, or nullopt when the server has no such file (HTTP 404/410).
Result<std::optional<std::wstring>> FetchSha256(const std::wstring& url);

// Downloads `url` to `destination` (via destination + ".part", renamed on success).
// When `expectedSha256` is not empty the digest is computed while downloading and must match,
// otherwise the .part file is discarded and `destination` is left untouched.
Result<> Download(const std::wstring& url, const std::filesystem::path& destination,
                  const ProgressFn& progress, std::stop_token stop, std::wstring_view expectedSha256 = {});

}  // namespace DL::Net
