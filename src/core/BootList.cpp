#include "core/BootList.h"
#include "support/Config.h"
#include "support/Text.h"
#include <algorithm>
#include <cwctype>
#include <format>

namespace DL::Boot {
namespace {

// "\EFI\debian\shimx64.efi" is below "EFI\debian" (case-insensitive, either slash).
bool UnderFolder(const std::wstring& path, std::wstring_view folder) {
    if (folder.empty()) return false;
    std::wstring_view p(path);
    while (!p.empty() && (p.front() == L'\\' || p.front() == L'/')) p.remove_prefix(1);
    if (p.size() <= folder.size() || (p[folder.size()] != L'\\' && p[folder.size()] != L'/')) return false;
    for (std::size_t i = 0; i < folder.size(); ++i) {
        const wchar_t a = p[i] == L'/' ? L'\\' : p[i], b = folder[i] == L'/' ? L'\\' : folder[i];
        if (std::towlower(a) != std::towlower(b)) return false;
    }
    return true;
}

}  // namespace

const Row* Snapshot::Find(std::uint16_t number) const {
    for (const auto& r : rows)
        if (r.entry.number == number) return &r;
    return nullptr;
}

const Row* Snapshot::Linux() const {
    const Row* unlisted = nullptr;
    for (const auto& r : rows) {
        if (!r.linux) continue;
        if (r.position >= 0) return &r;
        if (!unlisted) unlisted = &r;
    }
    return unlisted;
}

Result<Snapshot> Read(const Efi::Partition* esp, std::wstring_view espFolder, bool includeApps) {
    auto all = Efi::ListEntries();
    if (!all) return Fail(all.error());
    Snapshot s;
    s.current = Efi::BootCurrent();
    s.next = Efi::BootNext();
    std::vector<Row> unlisted;
    for (auto& e : *all) {
        Row r;
        r.position = e.inOrder ? static_cast<int>(e.orderPosition) : -1;
        r.current = s.current && *s.current == e.number;
        r.next = s.next && *s.next == e.number;
        r.linux = UnderFolder(e.filePath, espFolder) && (!esp || Efi::OnPartition(e, *esp));
        r.entry = std::move(e);
        if (r.position >= 0) {
            s.rows.push_back(std::move(r));
            continue;
        }
        // Firmware applications (Setup, diagnostics ...) and hidden options are noise unless asked for.
        if (!includeApps && !r.linux && (r.entry.Application() || r.entry.Hidden())) {
            ++s.hiddenApps;
            continue;
        }
        unlisted.push_back(std::move(r));
    }
    s.ordered = s.rows.size();
    std::ranges::sort(unlisted, {}, [](const Row& r) { return r.entry.number; });
    for (auto& r : unlisted) s.rows.push_back(std::move(r));
    return s;
}

std::optional<std::uint16_t> ParseNumber(std::wstring_view text) {
    std::wstring t;
    for (wchar_t c : text) t.push_back(static_cast<wchar_t>(std::towlower(c)));
    if (t.starts_with(L"boot")) t.erase(0, 4);
    else if (t.starts_with(L"0x")) t.erase(0, 2);
    if (t.empty() || t.size() > 4) return std::nullopt;
    unsigned value = 0;
    for (wchar_t c : t) {
        if (!std::iswxdigit(c)) return std::nullopt;
        value = value * 16 + static_cast<unsigned>(c <= L'9' ? c - L'0' : c - L'a' + 10);
    }
    return static_cast<std::uint16_t>(value);
}

std::wstring Name(std::uint16_t number) { return std::format(L"Boot{:04X}", number); }

std::wstring Target(const Efi::BootEntry& e) {
    if (e.filePath.empty()) return e.device.empty() ? Text::Get(IDS_NO_DEVICE_PATH) : e.device;
    return e.device.empty() ? e.filePath : e.device + L" " + e.filePath;
}

}  // namespace DL::Boot
