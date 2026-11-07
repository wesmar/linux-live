#include "install/Layout.h"
#include "install/EspInstaller.h"
#include "support/Text.h"
#include <Windows.h>
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <unordered_map>

namespace DL::Install {
namespace {
namespace fs = std::filesystem;

constexpr std::uint64_t kMaxEfiImage = 64ull << 20;  // grubx64.efi is ~3 MB; anything larger is not GRUB
constexpr std::uint64_t kMaxGrubCfg = 1ull << 20;

std::wstring Widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

bool EqualNoCase(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() && std::ranges::equal(a, b, [](wchar_t x, wchar_t y) { return std::towlower(x) == std::towlower(y); });
}

// Plain names separated by '/' or '\' - no drive, device, "." or ".." - joined with backslashes.
std::optional<std::wstring> SafeRelative(std::wstring_view path, std::size_t* parts = nullptr) {
    std::wstring out;
    std::size_t count = 0;
    for (std::size_t i = 0; i < path.size();) {
        while (i < path.size() && (path[i] == L'/' || path[i] == L'\\')) ++i;
        std::size_t j = i;
        while (j < path.size() && path[j] != L'/' && path[j] != L'\\') ++j;
        if (j == i) break;
        const std::wstring_view part = path.substr(i, j - i);
        if (part == L"." || part == L".." || part.find_first_of(L":*?\"<>|") != std::wstring_view::npos ||
            std::ranges::any_of(part, [](wchar_t c) { return c < 0x20; }))
            return std::nullopt;
        if (!out.empty()) out += L'\\';
        out += part;
        ++count;
        i = j;
    }
    if (out.empty()) return std::nullopt;
    if (parts) *parts = count;
    return out;
}

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

std::string Unquote(std::string_view v) {
    v = Trim(v);
    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) v = v.substr(1, v.size() - 2);
    return std::string(v);
}

template <class T>
bool ReadAt(std::span<const std::uint8_t> d, std::size_t at, T& out) {
    if (at > d.size() || d.size() - at < sizeof(T)) return false;
    std::memcpy(&out, d.data() + at, sizeof(T));
    return true;
}

std::optional<std::string> ReadSmallFile(const fs::path& p, std::uint64_t limit) {
    std::error_code ec;
    const auto size = fs::file_size(p, ec);
    if (ec || size > limit) return std::nullopt;
    std::ifstream in(p, std::ios::binary);
    std::string s(static_cast<std::size_t>(size), '\0');
    if (!in.read(s.data(), static_cast<std::streamsize>(s.size()))) return std::nullopt;
    return s;
}

}  // namespace

std::optional<std::wstring> GrubPrefix(std::span<const std::uint8_t> d) {
    std::uint32_t pe = 0, signature = 0;
    std::uint16_t sections = 0, optional = 0;
    if (!ReadAt(d, 0x3C, pe) || !ReadAt(d, pe, signature) || signature != 0x00004550 ||  // "PE\0\0"
        !ReadAt(d, std::size_t{pe} + 6, sections) || !ReadAt(d, std::size_t{pe} + 20, optional))
        return std::nullopt;
    const std::size_t table = std::size_t{pe} + 24 + optional;
    for (std::uint16_t i = 0; i < sections; ++i) {
        const std::size_t s = table + std::size_t{i} * 40;
        char name[8]{};
        std::uint32_t rawSize = 0, rawPtr = 0;
        if (!ReadAt(d, s, name) || !ReadAt(d, s + 16, rawSize) || !ReadAt(d, s + 20, rawPtr)) return std::nullopt;
        if (std::memcmp(name, "mods\0\0\0\0", 8) != 0) continue;
        // struct grub_module_info64 { magic "mimg", padding, offset, size } then the modules.
        std::uint32_t magic = 0;
        std::uint64_t offset = 0, size = 0;
        if (!ReadAt(d, rawPtr, magic) || magic != 0x676D696D || !ReadAt(d, std::size_t{rawPtr} + 8, offset) ||
            !ReadAt(d, std::size_t{rawPtr} + 16, size) || size > rawSize)
            return std::nullopt;
        std::size_t pos = std::size_t{rawPtr} + static_cast<std::size_t>(offset);
        const std::size_t end = std::size_t{rawPtr} + static_cast<std::size_t>(size);
        while (pos + 8 <= end) {
            std::uint32_t type = 0, length = 0;
            if (!ReadAt(d, pos, type) || !ReadAt(d, pos + 4, length) || length < 8 || length > end - pos) break;
            if (type == 3) {  // OBJ_TYPE_PREFIX, e.g. "/EFI/debian" or "(hd0,gpt1)/EFI/debian"
                std::string_view text(reinterpret_cast<const char*>(d.data() + pos + 8), length - 8);
                text = text.substr(0, text.find('\0'));
                if (!text.empty() && text.front() == '(') {
                    const std::size_t close = text.find(')');
                    if (close == std::string_view::npos) return std::nullopt;
                    text.remove_prefix(close + 1);
                }
                std::size_t parts = 0;
                auto folder = SafeRelative(Widen(text), &parts);
                // The ESP layout is EFI\<vendor>: exactly two levels below the root, starting with EFI.
                if (!folder || parts != 2 || !EqualNoCase(folder->substr(0, 4), L"EFI\\")) return std::nullopt;
                return folder;
            }
            pos += length;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

bool IsLiveConfig(std::string_view cfg) {
    return cfg.find("boot=live") != std::string_view::npos || cfg.find("rd.live.image") != std::string_view::npos ||
           cfg.find("root=live:") != std::string_view::npos;
}

bool SearchesKernel(std::string_view cfg) {
    for (std::size_t start = 0; start < cfg.size();) {
        std::size_t end = cfg.find('\n', start);
        if (end == std::string_view::npos) end = cfg.size();
        const std::string_view line = Trim(cfg.substr(start, end - start));
        start = end + 1;
        if (line.starts_with("search") && line.find("--file") != std::string_view::npos) return true;
    }
    return false;
}

std::optional<std::wstring> MediaFolder(std::string_view cfg) {
    std::unordered_map<std::string, std::string> vars;
    std::optional<std::string> value;
    for (std::size_t start = 0; start < cfg.size();) {
        std::size_t end = cfg.find('\n', start);
        if (end == std::string_view::npos) end = cfg.size();
        const std::string_view line = Trim(cfg.substr(start, end - start));
        start = end + 1;
        if (line.empty() || line.front() == '#') continue;
        if (line.starts_with("set ")) {
            const std::string_view assignment = Trim(line.substr(4));
            if (const std::size_t eq = assignment.find('='); eq != std::string_view::npos && !vars.contains(std::string(assignment.substr(0, eq))))
                vars.emplace(std::string(assignment.substr(0, eq)), Unquote(assignment.substr(eq + 1)));
            continue;
        }
        constexpr std::string_view key = "live-media-path=";
        if (const std::size_t at = line.find(key); at != std::string_view::npos && !value) {
            std::string_view v = line.substr(at + key.size());
            v = v.substr(0, v.find_first_of(" \t"));
            value = Unquote(v);
        }
    }
    if (!value) return std::wstring(L"live");  // live-boot looks in /live when nothing is given
    std::string resolved = *value;
    if (!resolved.empty() && resolved.front() == '$') {
        std::string name = resolved.substr(1);
        if (name.size() >= 2 && name.front() == '{' && name.back() == '}') name = name.substr(1, name.size() - 2);
        const auto it = vars.find(name);
        if (it == vars.end()) return std::nullopt;
        resolved = it->second;
    }
    return SafeRelative(Widen(resolved));
}

Result<Layout> ReadLayout(const fs::path& tarFile) {
    auto efi = ReadPackageFile(tarFile, L"grubx64.efi", kMaxEfiImage);
    if (!efi) return Fail(efi.error());
    const auto prefix = GrubPrefix(*efi);
    if (!prefix) return Fail(Text::Get(IDS_PACKAGE_NO_PREFIX));
    auto cfg = ReadPackageFile(tarFile, L"grub.cfg", kMaxGrubCfg);
    if (!cfg) return Fail(Text::Get(IDS_PACKAGE_NO_GRUBCFG));
    const std::string_view text(reinterpret_cast<const char*>(cfg->data()), cfg->size());
    if (!IsLiveConfig(text)) return Fail(Text::Get(IDS_PACKAGE_NO_GRUBCFG));
    const auto media = MediaFolder(text);
    if (!media) return Fail(Text::Get(IDS_PACKAGE_BAD_MEDIA));
    return Layout{*prefix, *media, SearchesKernel(text)};
}

bool BelongsToInstalledSystem(const fs::path& folder) {
    std::error_code ec;
    const fs::path cfg = folder / L"grub.cfg";
    if (!fs::is_regular_file(cfg, ec)) return false;
    const auto text = ReadSmallFile(cfg, kMaxGrubCfg);
    return !text || !IsLiveConfig(*text);  // unreadable or too large: do not touch it either
}

std::optional<Layout> FindDeployed(const fs::path& espRoot) {
    std::error_code ec;
    std::vector<fs::path> folders;
    for (const auto& entry : fs::directory_iterator(espRoot / L"EFI", ec))
        if (entry.is_directory(ec)) folders.push_back(entry.path());
    std::ranges::sort(folders);
    for (const auto& folder : folders) {
        const auto cfg = ReadSmallFile(folder / L"grub.cfg", kMaxGrubCfg);
        if (!cfg || !IsLiveConfig(*cfg)) continue;
        if (!fs::is_regular_file(folder / L"shimx64.efi", ec) && !fs::is_regular_file(folder / L"grubx64.efi", ec)) continue;
        const auto media = MediaFolder(*cfg);
        if (!media) continue;
        return Layout{L"EFI\\" + folder.filename().wstring(), *media, SearchesKernel(*cfg)};
    }
    return std::nullopt;
}

}  // namespace DL::Install
