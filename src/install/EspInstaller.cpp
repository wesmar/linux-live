#include "install/EspInstaller.h"
#include "support/Config.h"
#include "support/Text.h"
#include <Windows.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace DL::Install {
namespace fs = std::filesystem;
namespace {

constexpr std::size_t kBlock = 512;

struct Entry {
    fs::path relative;
    std::uint64_t size = 0;
    std::uint64_t offset = 0;  // data offset inside the tar
    std::int64_t mtime = 0;    // unix seconds
    bool directory = false;
};

std::wstring Widen(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::string CString(const char* p, std::size_t max) {
    std::size_t n = 0;
    while (n < max && p[n]) ++n;
    return std::string(p, n);
}

bool ParseNumber(const char* p, std::size_t len, std::uint64_t& out) {
    if (static_cast<unsigned char>(p[0]) & 0x80) {  // GNU base-256
        std::uint64_t v = static_cast<unsigned char>(p[0]) & 0x7F;
        for (std::size_t i = 1; i < len; ++i) {
            if (v >> 56) return false;
            v = (v << 8) | static_cast<unsigned char>(p[i]);
        }
        out = v;
        return true;
    }
    std::uint64_t v = 0;
    std::size_t i = 0;
    while (i < len && (p[i] == ' ' || p[i] == '\0')) ++i;
    for (; i < len && p[i] >= '0' && p[i] <= '7'; ++i) v = v * 8 + static_cast<std::uint64_t>(p[i] - '0');
    out = v;
    return true;
}

bool IsZero(const char* p) {
    return std::all_of(p, p + kBlock, [](char c) { return c == 0; });
}

bool ChecksumOk(const char* h) {
    std::uint64_t expected = 0;
    ParseNumber(h + 148, 8, expected);
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < kBlock; ++i) sum += (i >= 148 && i < 156) ? ' ' : static_cast<unsigned char>(h[i]);
    return sum == expected;
}

// "<len> key=value\n" records
void ParsePax(const std::string& data, std::string& path, std::optional<std::uint64_t>& size, std::optional<std::int64_t>& mtime) {
    std::size_t pos = 0;
    while (pos < data.size()) {
        const std::size_t space = data.find(' ', pos);
        if (space == std::string::npos) break;
        const auto len = static_cast<std::size_t>(std::strtoull(data.c_str() + pos, nullptr, 10));
        if (len == 0 || pos + len > data.size()) break;
        const std::string record = data.substr(space + 1, pos + len - space - 2);  // without trailing '\n'
        const std::size_t eq = record.find('=');
        if (eq != std::string::npos) {
            const std::string key = record.substr(0, eq), value = record.substr(eq + 1);
            if (key == "path") path = value;
            else if (key == "size") size = std::strtoull(value.c_str(), nullptr, 10);
            else if (key == "mtime") mtime = static_cast<std::int64_t>(std::strtod(value.c_str(), nullptr));
        }
        pos += len;
    }
}

Result<fs::path> SafePath(std::string_view name) {
    std::wstring wide = Widen(name);
    if (wide.empty()) return Fail(Text::Get(IDS_TAR_BAD_NAME));
    std::ranges::replace(wide, L'/', L'\\');
    fs::path result;
    std::size_t start = 0;
    while (start <= wide.size()) {
        std::size_t end = wide.find(L'\\', start);
        if (end == std::wstring::npos) end = wide.size();
        const std::wstring part = wide.substr(start, end - start);
        start = end + 1;
        if (part.empty() || part == L".") continue;
        if (part == L".." || part.find_first_of(L":*?\"<>|") != std::wstring::npos)
            return Fail(Text::Format(IDS_TAR_UNSAFE, wide));
        result /= part;
    }
    if (result.empty()) return fs::path();
    return result;
}

Result<std::vector<Entry>> ReadIndex(std::ifstream& in, std::uint64_t archiveSize) {
    std::vector<Entry> entries;
    std::vector<char> header(kBlock);
    std::string longName, paxPath;
    std::optional<std::uint64_t> paxSize;
    std::optional<std::int64_t> paxTime;
    std::uint64_t pos = 0;
    bool ended = false;

    while (pos + kBlock <= archiveSize) {
        in.seekg(static_cast<std::streamoff>(pos));
        in.read(header.data(), kBlock);
        if (!in) return Fail(Text::Get(IDS_TAR_READ));
        pos += kBlock;
        if (IsZero(header.data())) { ended = true; break; }
        if (!ChecksumOk(header.data())) return Fail(Text::Get(IDS_TAR_CHECKSUM));

        std::uint64_t size = 0, mtime = 0;
        ParseNumber(header.data() + 124, 12, size);
        ParseNumber(header.data() + 136, 12, mtime);
        const char type = header[156];
        std::string name = CString(header.data(), 100);
        if (std::memcmp(header.data() + 257, "ustar\0", 6) == 0) {  // POSIX ustar only (GNU format reuses the prefix field)
            const std::string prefix = CString(header.data() + 345, 155);
            if (!prefix.empty()) name = prefix + "/" + name;
        }

        auto readData = [&](std::uint64_t n, std::string& out) -> bool {
            if (n > (1u << 20) || pos + n > archiveSize) return false;
            out.resize(static_cast<std::size_t>(n));
            in.seekg(static_cast<std::streamoff>(pos));
            in.read(out.data(), static_cast<std::streamsize>(n));
            return static_cast<bool>(in);
        };
        const std::uint64_t padded = (size + kBlock - 1) / kBlock * kBlock;

        if (type == 'L') {
            if (!readData(size, longName)) return Fail(Text::Get(IDS_TAR_LONGNAME));
            longName = CString(longName.c_str(), longName.size());
            pos += padded;
            continue;
        }
        if (type == 'x') {
            std::string data;
            if (!readData(size, data)) return Fail(Text::Get(IDS_TAR_PAX));
            ParsePax(data, paxPath, paxSize, paxTime);
            pos += padded;
            continue;
        }
        if (type == 'g') { pos += padded; continue; }

        if (!longName.empty()) name = longName;
        if (!paxPath.empty()) name = paxPath;
        if (paxSize) size = *paxSize;
        const std::int64_t modified = paxTime ? *paxTime : static_cast<std::int64_t>(mtime);
        longName.clear(); paxPath.clear(); paxSize.reset(); paxTime.reset();

        const bool isDir = type == '5';
        if (!isDir && type != '0' && type != '\0')
            return Fail(Text::Format(IDS_TAR_TYPE, static_cast<wchar_t>(type)));
        auto path = SafePath(name);
        if (!path) return Fail(path.error());
        const std::uint64_t dataPadded = (size + kBlock - 1) / kBlock * kBlock;
        if (!isDir && pos + size > archiveSize) return Fail(Text::Get(IDS_TAR_TRUNCATED));
        if (!path->empty()) entries.push_back({*path, isDir ? 0 : size, pos, modified, isDir});
        pos += isDir ? 0 : dataPadded;
    }
    if (!ended && pos < archiveSize) return Fail(Text::Get(IDS_TAR_TRUNCATED));
    return entries;
}

constexpr std::int64_t kEpochDiff = 116444736000000000LL;  // 1601 -> 1970 in 100 ns

FILETIME ToFileTime(std::int64_t unixSeconds) {
    const std::int64_t t = unixSeconds * 10000000LL + kEpochDiff;
    return {static_cast<DWORD>(t & 0xFFFFFFFF), static_cast<DWORD>(t >> 32)};
}

bool Unchanged(const fs::path& target, std::uint64_t size, std::int64_t mtime) {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(target.c_str(), GetFileExInfoStandard, &info)) return false;
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        ((static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow) != size)
        return false;
    const std::int64_t have = ((static_cast<std::int64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) |
                               info.ftLastWriteTime.dwLowDateTime);
    const std::int64_t want = mtime * 10000000LL + kEpochDiff;
    return std::abs(have - want) <= 20000000LL;  // FAT keeps 2 s resolution
}

// Closes the handle and deletes the temporary file unless it was committed (tempPath cleared).
struct FileHandle {
    HANDLE h = INVALID_HANDLE_VALUE;
    std::wstring tempPath;
    FileHandle() = default;
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    ~FileHandle() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        if (!tempPath.empty()) DeleteFileW(tempPath.c_str());
    }
};

}  // namespace

Result<std::wstring> FindLoader(const fs::path& dir, std::wstring_view espFolder) {
    std::error_code ec;
    auto toEfiPath = [&](const fs::path& p) { return std::wstring(L"\\") + std::wstring(espFolder) + L"\\" + p.filename().wstring(); };
    for (auto name : Config::kLoaderCandidates) {
        const fs::path candidate = dir / name;
        if (fs::is_regular_file(candidate, ec)) return toEfiPath(candidate);
    }
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        std::wstring ext = entry.path().extension().wstring();
        for (auto& c : ext) c = static_cast<wchar_t>(std::towlower(c));
        if (entry.is_regular_file(ec) && ext == L".efi") return toEfiPath(entry.path());
    }
    return Fail(Text::Format(IDS_NO_LOADER, dir.wstring()));
}

Result<std::vector<std::uint8_t>> ReadPackageFile(const fs::path& tarFile, std::wstring_view name, std::uint64_t limit) {
    std::error_code ec;
    const std::uint64_t archiveSize = fs::file_size(tarFile, ec);
    if (ec) return Fail(Text::Format(IDS_PACKAGE_OPEN, tarFile.wstring()));
    std::ifstream in(tarFile, std::ios::binary);
    if (!in) return Fail(Text::Format(IDS_PACKAGE_OPEN, tarFile.wstring()));
    auto index = ReadIndex(in, archiveSize);
    if (!index) return Fail(index.error());
    for (const auto& e : *index) {
        if (e.directory || _wcsicmp(e.relative.filename().c_str(), std::wstring(name).c_str()) != 0) continue;
        if (e.size > limit) return Fail(Text::Format(IDS_PACKAGE_OPEN, tarFile.wstring()));
        std::vector<std::uint8_t> data(static_cast<std::size_t>(e.size));
        in.clear();
        in.seekg(static_cast<std::streamoff>(e.offset));
        if (!in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()))) return Fail(Text::Get(IDS_TAR_READ));
        return data;
    }
    return Fail(Text::Format(IDS_PACKAGE_MISSING_FILE, name));
}

namespace {

// The package index with an "EFI/<folder>/" prefix stripped when every file carries it.
Result<std::vector<Entry>> PackageIndex(std::ifstream& in, std::uint64_t archiveSize, std::wstring_view espFolder) {
    auto index = ReadIndex(in, archiveSize);
    if (!index) return Fail(index.error());
    const fs::path prefix(espFolder);
    auto startsWithPrefix = [&](const Entry& e) {
        auto a = e.relative.begin();
        for (auto b = prefix.begin(); b != prefix.end(); ++b, ++a)
            if (a == e.relative.end() || _wcsicmp(a->c_str(), b->c_str()) != 0) return false;
        return true;
    };
    const bool stripPrefix = !index->empty() && std::ranges::all_of(*index, [&](const Entry& e) { return e.directory || startsWithPrefix(e); });
    if (stripPrefix) {
        std::vector<Entry> kept;
        kept.reserve(index->size());
        const auto prefixDist = static_cast<std::ptrdiff_t>(std::distance(prefix.begin(), prefix.end()));
        for (auto& e : *index) {
            if (!startsWithPrefix(e)) continue;  // directories outside the prefix
            fs::path rest;
            auto it = e.relative.begin();
            std::advance(it, prefixDist);
            for (; it != e.relative.end(); ++it) rest /= *it;
            if (rest.empty()) continue;
            e.relative = std::move(rest);
            kept.push_back(std::move(e));
        }
        *index = std::move(kept);
    }
    std::erase_if(*index, [](const Entry& e) { return e.directory; });
    for (const auto& e : *index)
        if (e.size > 0xFFFFFFFFull) return Fail(Text::Format(IDS_PACKAGE_FAT_LIMIT, e.relative.wstring()));
    if (index->empty()) return Fail(Text::Get(IDS_PACKAGE_EMPTY));
    return index;
}

std::uint64_t ExistingSize(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec) ? fs::file_size(p, ec) : 0;
}

std::uint64_t FreeSpace(const std::wstring& root) {
    ULARGE_INTEGER free{};
    return GetDiskFreeSpaceExW(root.c_str(), &free, nullptr, nullptr) ? free.QuadPart : ~0ull;
}

}  // namespace

bool LoaderFile(const fs::path& name) {
    std::wstring ext = name.extension().wstring();
    for (auto& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    return ext == L".efi" || ext == L".csv" || _wcsicmp(name.filename().c_str(), L"grub.cfg") == 0;
}

Result<EspPlan> PlanEsp(const fs::path& tarFile, const Destination& esp, std::wstring_view espFolder) {
    std::error_code ec;
    const std::uint64_t archiveSize = fs::file_size(tarFile, ec);
    if (ec) return Fail(Text::Format(IDS_PACKAGE_OPEN, tarFile.wstring()));
    std::ifstream in(tarFile, std::ios::binary);
    if (!in) return Fail(Text::Format(IDS_PACKAGE_OPEN, tarFile.wstring()));
    auto index = PackageIndex(in, archiveSize, espFolder);
    if (!index) return Fail(index.error());
    EspPlan plan;
    plan.reserve = esp.reserve;
    plan.freeBytes = FreeSpace(esp.volumeRoot);
    for (const auto& e : *index) {
        const std::uint64_t existing = ExistingSize(esp.dir / e.relative);
        const std::uint64_t growth = e.size > existing ? e.size - existing : 0;
        if (LoaderFile(e.relative)) {
            plan.loaderGrowth += growth;
        } else {
            plan.kernelGrowth += growth;
            plan.kernelBytes += e.size;
            plan.kernelOnEsp += existing;
        }
    }
    return plan;
}

Result<Report> DeployEsp(const fs::path& tarFile, const Destination& esp, const Destination* kernel, std::wstring_view espFolder,
                         const ProgressFn& progress, const LogFn& log, std::stop_token stop) {
    std::error_code ec;
    const std::uint64_t archiveSize = fs::file_size(tarFile, ec);
    if (ec) return Fail(Text::Format(IDS_PACKAGE_OPEN, tarFile.wstring()));
    std::ifstream in(tarFile, std::ios::binary);
    if (!in) return Fail(Text::Format(IDS_PACKAGE_OPEN, tarFile.wstring()));
    auto index = PackageIndex(in, archiveSize, espFolder);
    if (!index) return Fail(index.error());

    // Where each file goes, and the space that needs - checked before anything is written.
    auto toKernel = [&](const Entry& e) { return kernel != nullptr && !LoaderFile(e.relative); };
    std::uint64_t total = 0, espGrowth = 0, kernelGrowth = 0, freedOnEsp = 0, espBytes = 0, kernelBytes = 0;
    std::size_t espFiles = 0, kernelFiles = 0;
    for (const auto& e : *index) {
        total += e.size;
        if (toKernel(e)) {
            const std::uint64_t existing = ExistingSize(kernel->dir / e.relative);
            kernelGrowth += e.size > existing ? e.size - existing : 0;
            ++kernelFiles;
            kernelBytes += e.size;
            freedOnEsp += ExistingSize(esp.dir / e.relative);  // the old copy on the ESP is deleted
        } else {
            const std::uint64_t existing = ExistingSize(esp.dir / e.relative);
            espGrowth += e.size > existing ? e.size - existing : 0;
            ++espFiles;
            espBytes += e.size;
        }
    }
    // Firmware updates stage their capsules on the ESP: what grows must leave the reserve free.
    if (const std::uint64_t free = FreeSpace(esp.volumeRoot); espGrowth && espGrowth + esp.reserve > free + freedOnEsp)
        return Fail(Text::Format(IDS_ESP_NO_SPACE_RESERVE, FormatBytes(espGrowth), FormatBytes(esp.reserve), FormatBytes(free + freedOnEsp)));
    if (kernel)
        if (const std::uint64_t free = FreeSpace(kernel->volumeRoot); kernelGrowth + kernel->reserve > free)
            return Fail(Text::Format(IDS_BOOTPART_NO_SPACE, FormatBytes(kernelGrowth), FormatBytes(free)));

    Report report;
    std::uint64_t done = 0;
    std::vector<char> buffer(1 << 20);
    auto write = [&](const Entry& e, const fs::path& dir) -> Result<> {
        if (stop.stop_requested()) return Fail(Text::Get(IDS_CANCELED));
        const fs::path target = dir / e.relative;
        fs::create_directories(target.parent_path(), ec);
        if (Unchanged(target, e.size, e.mtime)) {
            done += e.size;
            ++report.skipped;
            log(Text::Format(IDS_ESP_UNCHANGED, e.relative.wstring()));
            progress(done, total);
            return {};
        }
        fs::path temp = target;
        temp += L".tmp";
        FileHandle out;
        out.h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (out.h == INVALID_HANDLE_VALUE) return Fail(ErrorText(Text::Format(IDS_CTX_CREATE, temp.wstring()), GetLastError()));
        out.tempPath = temp.wstring();  // removed again by ~FileHandle on any early return

        in.clear();
        in.seekg(static_cast<std::streamoff>(e.offset));
        for (std::uint64_t left = e.size; left;) {
            if (stop.stop_requested()) return Fail(Text::Get(IDS_CANCELED));
            const std::size_t chunk = static_cast<std::size_t>(std::min<std::uint64_t>(left, buffer.size()));
            in.read(buffer.data(), static_cast<std::streamsize>(chunk));
            if (!in) return Fail(Text::Get(IDS_TAR_READ));
            DWORD written = 0;
            if (!WriteFile(out.h, buffer.data(), static_cast<DWORD>(chunk), &written, nullptr) || written != chunk)
                return Fail(ErrorText(Text::Format(IDS_CTX_WRITE, temp.wstring()), GetLastError()));
            left -= chunk;
            done += chunk;
            progress(done, total);
        }
        const FILETIME ft = ToFileTime(e.mtime);
        SetFileTime(out.h, &ft, nullptr, &ft);
        if (!FlushFileBuffers(out.h)) return Fail(ErrorText(Text::Format(IDS_CTX_FLUSH, temp.wstring()), GetLastError()));
        CloseHandle(out.h);
        out.h = INVALID_HANDLE_VALUE;
        if (!MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING))
            return Fail(ErrorText(Text::Format(IDS_CTX_REPLACE, target.wstring()), GetLastError()));
        out.tempPath.clear();  // committed
        ++report.files;
        report.bytes += e.size;
        log(std::format(L"  {} ({})", e.relative.wstring(), FormatBytes(e.size)));
        return {};
    };

    // Kernel files first: they have to be in place before the ESP stops offering its own copies.
    if (kernel) {
        fs::create_directories(kernel->dir, ec);
        if (ec) return Fail(Text::Format(IDS_CANNOT_CREATE, kernel->dir.wstring()));
        log(Text::Format(IDS_ESP_DEPLOYING, kernelFiles, FormatBytes(kernelBytes), kernel->dir.wstring()));
        for (const auto& e : *index)
            if (toKernel(e))
                if (auto r = write(e, kernel->dir); !r) return Fail(r.error());
        // GRUB loads the kernel from its own folder when it is there; only without it does grub.cfg
        // search the boot partition. So the copies from an earlier deployment have to go.
        bool removed = false;
        for (const auto& e : *index)
            if (toKernel(e) && fs::remove(esp.dir / e.relative, ec)) removed = true;
        if (removed) log(Text::Format(IDS_DEPLOY_KERNEL_MOVED, espFolder));
        report.split = true;
    }

    fs::create_directories(esp.dir, ec);
    if (ec) return Fail(Text::Format(IDS_CANNOT_CREATE, esp.dir.wstring()));
    log(Text::Format(IDS_ESP_DEPLOYING, espFiles, FormatBytes(espBytes), esp.dir.wstring()));
    for (const auto& e : *index)
        if (!toKernel(e))
            if (auto r = write(e, esp.dir); !r) return Fail(r.error());
    progress(total, total);

    if (auto loader = FindLoader(esp.dir, espFolder)) report.loaderPath = *loader;
    return report;
}

}  // namespace DL::Install

