#include "efi/Efi.h"
#include "support/Text.h"
#include <winioctl.h>
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <format>
#include <memory>
#include <span>
#include <type_traits>

namespace DL::Efi {
namespace {

constexpr wchar_t kGlobalGuid[] = L"{8BE4DF61-93CA-11D2-AA0D-00E098032B8C}";  // EFI_GLOBAL_VARIABLE
constexpr DWORD kVarAttributes = 0x7;  // NON_VOLATILE | BOOTSERVICE_ACCESS | RUNTIME_ACCESS
constexpr int kScanLimit = 0x400;
constexpr GUID kEspType{0xC12A7328, 0xF81F, 0x11D2, {0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B}};

// ---------------------------------------------------------------------------
// Handles / IOCTL helpers
// ---------------------------------------------------------------------------
struct HandleCloser {
    void operator()(HANDLE h) const noexcept {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};
using Handle = std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleCloser>;

Result<Handle> OpenDevice(const std::wstring& path) {
    Handle h(CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
    if (h.get() == INVALID_HANDLE_VALUE) return Fail(ErrorText(Text::Format(IDS_CTX_OPEN, path), GetLastError()));
    return h;
}

// ---------------------------------------------------------------------------
// NVRAM variable access
// ---------------------------------------------------------------------------
Result<std::optional<std::vector<std::uint8_t>>> ReadVar(const std::wstring& name) {
    std::vector<std::uint8_t> buffer(1024);
    for (;;) {
        SetLastError(0);
        const DWORD n = GetFirmwareEnvironmentVariableExW(name.c_str(), kGlobalGuid, buffer.data(),
                                                          static_cast<DWORD>(buffer.size()), nullptr);
        if (n) {
            buffer.resize(n);
            return std::optional<std::vector<std::uint8_t>>(std::move(buffer));
        }
        const DWORD error = GetLastError();
        if (error == ERROR_SUCCESS) return std::optional<std::vector<std::uint8_t>>(std::vector<std::uint8_t>{});  // exists, zero length
        if (error == ERROR_INSUFFICIENT_BUFFER && buffer.size() < (1u << 20)) {
            buffer.resize(buffer.size() * 4);
            continue;
        }
        if (error == ERROR_ENVVAR_NOT_FOUND) return std::optional<std::vector<std::uint8_t>>{};
        return Fail(ErrorText(Text::Format(IDS_CTX_READ_VARIABLE, name), error));
    }
}

Result<> WriteVar(const std::wstring& name, std::span<const std::uint8_t> data) {
    // Size 0 deletes the variable. The attributes must still be the variable's own (NV+BS+RT):
    // Windows rejects a delete with attributes 0 as ERROR_INVALID_PARAMETER (seen on Dell firmware).
    const BOOL ok = SetFirmwareEnvironmentVariableExW(name.c_str(), kGlobalGuid,
                                                      data.empty() ? nullptr : const_cast<std::uint8_t*>(data.data()),
                                                      static_cast<DWORD>(data.size()), kVarAttributes);
    if (!ok) {
        const DWORD error = GetLastError();
        // Deleting a variable that is already gone is success.
        if (data.empty() && error == ERROR_ENVVAR_NOT_FOUND) return {};
        return Fail(ErrorText(Text::Format(data.empty() ? IDS_CTX_DELETE_VARIABLE : IDS_CTX_WRITE_VARIABLE, name), error));
    }
    return {};
}

std::wstring BootName(std::uint16_t number) { return std::format(L"Boot{:04X}", number); }

Result<std::vector<std::uint16_t>> ReadU16List(const std::wstring& name) {
    auto var = ReadVar(name);
    if (!var) return Fail(var.error());
    std::vector<std::uint16_t> list;
    if (*var) {
        list.resize((*var)->size() / 2);
        std::memcpy(list.data(), (*var)->data(), list.size() * 2);
    }
    return list;
}

Result<> WriteU16List(const std::wstring& name, const std::vector<std::uint16_t>& list) {
    return WriteVar(name, {reinterpret_cast<const std::uint8_t*>(list.data()), list.size() * 2});
}

// ---------------------------------------------------------------------------
// EFI_LOAD_OPTION encode / decode
// ---------------------------------------------------------------------------
template <class T>
void Put(std::vector<std::uint8_t>& out, const T& value) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&value);
    out.insert(out.end(), p, p + sizeof(T));
}

std::vector<std::uint8_t> BuildLoadOption(const Partition& part, std::wstring_view loaderPath,
                                          std::wstring_view description) {
    // Device path: HD(...) / File(...) / End
    std::vector<std::uint8_t> path;

    // Media Hard Drive node (type 4, subtype 1, length 42)
    Put<std::uint8_t>(path, 4);
    Put<std::uint8_t>(path, 1);
    Put<std::uint16_t>(path, 42);
    Put<std::uint32_t>(path, part.number);
    Put<std::uint64_t>(path, part.startLba);
    Put<std::uint64_t>(path, part.sizeLba);
    std::uint8_t signature[16]{};
    if (part.gpt) std::memcpy(signature, &part.id, 16);
    else std::memcpy(signature, &part.mbrSignature, 4);
    path.insert(path.end(), signature, signature + 16);
    Put<std::uint8_t>(path, part.gpt ? 2 : 1);  // partition format: 1 = MBR, 2 = GPT
    Put<std::uint8_t>(path, part.gpt ? 2 : 1);  // signature type: 1 = MBR, 2 = GUID

    // Media File Path node (type 4, subtype 4)
    const std::size_t pathBytes = (loaderPath.size() + 1) * sizeof(wchar_t);
    Put<std::uint8_t>(path, 4);
    Put<std::uint8_t>(path, 4);
    Put<std::uint16_t>(path, static_cast<std::uint16_t>(4 + pathBytes));
    for (wchar_t c : loaderPath) Put<std::uint16_t>(path, static_cast<std::uint16_t>(c));
    Put<std::uint16_t>(path, 0);

    // End of device path
    Put<std::uint8_t>(path, 0x7F);
    Put<std::uint8_t>(path, 0xFF);
    Put<std::uint16_t>(path, 4);

    std::vector<std::uint8_t> option;
    Put<std::uint32_t>(option, kOptionActive);
    Put<std::uint16_t>(option, static_cast<std::uint16_t>(path.size()));
    for (wchar_t c : description) Put<std::uint16_t>(option, static_cast<std::uint16_t>(c));
    Put<std::uint16_t>(option, 0);
    option.insert(option.end(), path.begin(), path.end());
    return option;
}

// Short name of a device-path node (UEFI spec, chapter 10) for entries without an HD()/File() pair.
std::wstring NodeName(std::uint8_t type, std::uint8_t subtype) {
    switch (type) {
    case 1:
        if (subtype == 1) return L"PCI";
        if (subtype == 4) return L"Vendor HW";
        return L"Hardware";
    case 2: return L"ACPI";
    case 3:
        switch (subtype) {
        case 2: return L"SCSI";
        case 5: case 15: case 16: return L"USB";
        case 11: return L"Network (MAC)";
        case 12: return L"Network (IPv4)";
        case 13: return L"Network (IPv6)";
        case 18: return L"SATA";
        case 23: return L"NVMe";
        case 24: return L"HTTP boot";
        case 10: return L"Vendor messaging";
        default: return L"Messaging";
        }
    case 4:
        switch (subtype) {
        case 2: return L"CD-ROM";
        case 3: return L"Vendor media";
        case 6: return L"Firmware app";
        case 7: return L"Firmware volume";
        default: return L"Media";
        }
    case 5: return L"Legacy BBS";
    default: return std::format(L"Node {}/{}", type, subtype);
    }
}

std::optional<BootEntry> ParseLoadOption(std::uint16_t number, std::span<const std::uint8_t> d) {
    if (d.size() < 8) return std::nullopt;
    BootEntry entry;
    entry.number = number;
    std::uint16_t listLength = 0;
    std::memcpy(&entry.attributes, d.data(), 4);
    std::memcpy(&listLength, d.data() + 4, 2);

    std::size_t pos = 6;
    bool terminated = false;
    while (pos + 2 <= d.size()) {
        std::uint16_t c;
        std::memcpy(&c, d.data() + pos, 2);
        pos += 2;
        if (!c) { terminated = true; break; }
        entry.description.push_back(static_cast<wchar_t>(c));
    }
    if (!terminated || pos + listLength > d.size()) return std::nullopt;

    const std::size_t end = pos + listLength;
    std::uint8_t lastType = 0, lastSubtype = 0;
    bool haveHd = false;
    while (pos + 4 <= end) {
        const std::uint8_t type = d[pos], subtype = d[pos + 1];
        std::uint16_t length;
        std::memcpy(&length, d.data() + pos + 2, 2);
        if (length < 4 || pos + length > end) break;
        if (type == 4 && subtype == 4) {
            for (std::size_t i = pos + 4; i + 2 <= pos + length; i += 2) {
                std::uint16_t c;
                std::memcpy(&c, d.data() + i, 2);
                if (!c) break;
                entry.filePath.push_back(static_cast<wchar_t>(c));
            }
        }
        if (type == 4 && subtype == 1 && length >= 42) {  // HD()
            std::memcpy(&entry.partitionNumber, d.data() + pos + 4, 4);
            const std::uint8_t signatureType = d[pos + 41];
            if (signatureType == 2) {  // GUID
                std::memcpy(&entry.partitionGuid, d.data() + pos + 24, 16);
                entry.hasPartitionGuid = true;
            } else if (signatureType == 1) {  // MBR signature
                std::memcpy(&entry.mbrSignature, d.data() + pos + 24, 4);
                entry.hasMbrSignature = true;
            }
        }
        if (type == 0x7F && subtype == 0xFF) break;
        if (type == 4 && subtype == 1) haveHd = true;
        else if (!(type == 4 && subtype == 4)) { lastType = type; lastSubtype = subtype; }
        pos += length;
    }
    // Optional data written by Windows: "WINDOWS\0", a small header, then "BCDOBJECT={guid}" in UTF-16.
    if (constexpr std::string_view kWindows("WINDOWS\0", 8); d.size() >= end + kWindows.size() &&
                                                             std::memcmp(d.data() + end, kWindows.data(), kWindows.size()) == 0) {
        std::wstring text;
        for (std::size_t i = end + kWindows.size(); i + 2 <= d.size(); i += 2) {
            std::uint16_t c;
            std::memcpy(&c, d.data() + i, 2);
            text.push_back(c >= 0x20 && c < 0x7F ? static_cast<wchar_t>(c) : L' ');
        }
        constexpr std::wstring_view kKey = L"BCDOBJECT=";
        if (const std::size_t at = text.find(kKey); at != std::wstring::npos) {
            const std::size_t open = at + kKey.size(), close = text.find(L'}', open);
            if (close != std::wstring::npos && text[open] == L'{' && close - open == 37) entry.bcdObject = text.substr(open, close - open + 1);
        }
    }
    if (haveHd)
        entry.device = std::format(L"HD({},{})", entry.partitionNumber,
                                   entry.hasPartitionGuid ? L"GPT" : entry.hasMbrSignature ? L"MBR" : L"?");
    else if (lastType)
        entry.device = NodeName(lastType, lastSubtype);
    return entry;
}

}  // namespace

Result<std::optional<BootEntry>> ReadEntry(std::uint16_t number) {
    auto var = ReadVar(BootName(number));
    if (!var) return Fail(var.error());
    if (!*var) return std::optional<BootEntry>{};
    return ParseLoadOption(number, **var);
}

// ---------------------------------------------------------------------------
// Environment
// ---------------------------------------------------------------------------
bool IsUefi() {
    FIRMWARE_TYPE type = FirmwareTypeUnknown;
    return GetFirmwareType(&type) && type == FirmwareTypeUefi;
}

Result<> EnablePrivilege(const wchar_t* name) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return Fail(ErrorText(L"OpenProcessToken", GetLastError()));
    Handle guard(token);
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid))
        return Fail(ErrorText(L"LookupPrivilegeValue", GetLastError()));
    AdjustTokenPrivileges(token, FALSE, &tp, sizeof tp, nullptr, nullptr);
    if (const DWORD e = GetLastError(); e != ERROR_SUCCESS)
        return Fail(ErrorText(Text::Format(IDS_CTX_PRIVILEGE, name), e));
    return {};
}

std::optional<bool> SecureBootEnabled() {
    auto var = ReadVar(L"SecureBoot");
    if (!var || !*var || (*var)->empty()) return std::nullopt;
    return (**var)[0] != 0;
}

// ---------------------------------------------------------------------------
// ESP discovery
// ---------------------------------------------------------------------------
Result<Partition> QueryPartition(const std::wstring& volumeRoot) {
    std::wstring volumePath = volumeRoot;
    while (!volumePath.empty() && volumePath.back() == L'\\') volumePath.pop_back();  // CreateFile wants no trailing slash
    auto volume = OpenDevice(volumePath);
    if (!volume) return Fail(volume.error());

    VOLUME_DISK_EXTENTS extents{};
    DWORD returned = 0;
    if (!DeviceIoControl(volume->get(), IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, &extents,
                         sizeof extents, &returned, nullptr))
        return Fail(ErrorText(Text::Get(IDS_CTX_VOLUME_EXTENTS), GetLastError()));
    if (extents.NumberOfDiskExtents != 1) return Fail(Text::Get(IDS_VOLUME_SPANS));

    const DWORD diskNumber = extents.Extents[0].DiskNumber;
    const LONGLONG volumeOffset = extents.Extents[0].StartingOffset.QuadPart;
    auto disk = OpenDevice(std::format(L"\\\\.\\PhysicalDrive{}", diskNumber));
    if (!disk) return Fail(disk.error());

    DISK_GEOMETRY_EX geometry{};
    if (!DeviceIoControl(disk->get(), IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geometry,
                         sizeof geometry, &returned, nullptr))
        return Fail(ErrorText(Text::Get(IDS_CTX_GEOMETRY), GetLastError()));
    const std::uint64_t sector = geometry.Geometry.BytesPerSector ? geometry.Geometry.BytesPerSector : 512;

    std::vector<std::uint8_t> buffer(16 * 1024);
    for (;;) {
        if (DeviceIoControl(disk->get(), IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0, buffer.data(),
                            static_cast<DWORD>(buffer.size()), &returned, nullptr))
            break;
        const DWORD e = GetLastError();
        if (e == ERROR_INSUFFICIENT_BUFFER && buffer.size() < (1u << 22)) { buffer.resize(buffer.size() * 4); continue; }
        return Fail(ErrorText(Text::Get(IDS_CTX_LAYOUT), e));
    }

    const auto* layout = reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX*>(buffer.data());
    for (DWORD i = 0; i < layout->PartitionCount; ++i) {
        const PARTITION_INFORMATION_EX& p = layout->PartitionEntry[i];
        if (p.StartingOffset.QuadPart != volumeOffset) continue;
        Partition result;
        result.disk = diskNumber;
        result.number = p.PartitionNumber;
        result.startLba = static_cast<std::uint64_t>(p.StartingOffset.QuadPart) / sector;
        result.sizeBytes = static_cast<std::uint64_t>(p.PartitionLength.QuadPart);
        result.sizeLba = result.sizeBytes / sector;
        if (layout->PartitionStyle == PARTITION_STYLE_GPT) {
            result.gpt = true;
            result.id = p.Gpt.PartitionId;
            result.isEsp = std::memcmp(&p.Gpt.PartitionType, &kEspType, sizeof(GUID)) == 0;
        } else if (layout->PartitionStyle == PARTITION_STYLE_MBR) {
            result.mbrSignature = layout->Mbr.Signature;
            result.isEsp = p.Mbr.PartitionType == 0xEF;
        } else {
            return Fail(Text::Get(IDS_UNSUPPORTED_STYLE));
        }
        return result;
    }
    return Fail(Text::Get(IDS_PARTITION_NOT_FOUND));
}

// ---------------------------------------------------------------------------
// NVRAM boot entries
// ---------------------------------------------------------------------------
Result<std::vector<BootEntry>> ListEntries() {
    auto order = ReadU16List(L"BootOrder");
    if (!order) return Fail(order.error());
    std::vector<BootEntry> entries;
    for (std::size_t i = 0; i < order->size(); ++i) {
        auto e = ReadEntry((*order)[i]);
        if (!e) return Fail(e.error());
        if (!*e) continue;
        (*e)->inOrder = true;
        (*e)->orderPosition = i;
        entries.push_back(std::move(**e));
    }
    for (int n = 0; n < kScanLimit; ++n) {
        if (std::ranges::contains(*order, static_cast<std::uint16_t>(n))) continue;
        auto e = ReadEntry(static_cast<std::uint16_t>(n));
        if (!e) return Fail(e.error());
        if (*e) entries.push_back(std::move(**e));
    }
    return entries;
}

namespace {
bool EqualNoCase(std::wstring_view a, std::wstring_view b) {
    return std::ranges::equal(a, b, [](wchar_t x, wchar_t y) { return std::towlower(x) == std::towlower(y); });
}
}  // namespace

bool OnPartition(const BootEntry& e, const Partition& esp) {
    if (esp.gpt) return e.hasPartitionGuid && std::memcmp(&e.partitionGuid, &esp.id, sizeof(GUID)) == 0;
    return e.hasMbrSignature && e.mbrSignature == esp.mbrSignature && e.partitionNumber == esp.number;
}

std::vector<BootEntry> FilterEntries(const std::vector<BootEntry>& all, const Partition& esp,
                                     std::wstring_view directory) {
    std::vector<BootEntry> matches;
    for (const auto& e : all) {
        if (!OnPartition(e, esp)) continue;
        std::wstring_view path = e.filePath;
        if (path.size() > directory.size() && EqualNoCase(path.substr(0, directory.size()), directory) &&
            path[directory.size()] == L'\\')
            matches.push_back(e);
    }
    return matches;
}

Result<std::vector<BootEntry>> FindEntries(const Partition& esp, std::wstring_view directory) {
    auto all = ListEntries();
    if (!all) return Fail(all.error());
    return FilterEntries(*all, esp, directory);
}
std::optional<std::uint16_t> BootCurrent() {
    auto v = ReadU16List(L"BootCurrent");
    if (!v || v->empty()) return std::nullopt;
    return v->front();
}

std::optional<std::uint16_t> BootNext() {
    auto v = ReadU16List(L"BootNext");
    if (!v || v->empty()) return std::nullopt;
    return v->front();
}

Result<std::vector<std::uint16_t>> ReadBootOrder() { return ReadU16List(L"BootOrder"); }

Result<> WriteBootOrder(const std::vector<std::uint16_t>& order) {
    if (order.empty()) return Fail(Text::Get(IDS_EMPTY_ORDER));
    return WriteU16List(L"BootOrder", order);
}

Result<EntryResult> EnsureEntry(const Partition& esp, std::wstring_view loaderPath,
                                std::wstring_view description) {
    auto all = ListEntries();
    if (!all) return Fail(all.error());
    auto order = ReadU16List(L"BootOrder");
    if (!order) return Fail(order.error());

    // Reuse an equivalent entry (same ESP, same loader) exactly as it is.
    for (const auto& e : *all) {
        if (!OnPartition(e, esp) || !EqualNoCase(e.filePath, loaderPath)) continue;
        if (!std::ranges::contains(*order, e.number)) {
            order->push_back(e.number);
            if (auto w = WriteU16List(L"BootOrder", *order); !w) return Fail(w.error());
        }
        return EntryResult{e.number, false};
    }

    std::uint16_t number = 0;
    bool found = false;
    for (std::uint32_t n = 0; n <= 0xFFFF && !found; ++n) {
        auto var = ReadVar(BootName(static_cast<std::uint16_t>(n)));
        if (!var) return Fail(var.error());
        if (!*var) { number = static_cast<std::uint16_t>(n); found = true; }
    }
    if (!found) return Fail(Text::Get(IDS_NO_FREE_NUMBER));

    const auto option = BuildLoadOption(esp, loaderPath, description);
    if (auto w = WriteVar(BootName(number), option); !w) return Fail(w.error());
    order->push_back(number);
    if (auto w = WriteU16List(L"BootOrder", *order); !w) return Fail(w.error());
    return EntryResult{number, true};
}
Result<> SetBootNext(std::uint16_t number) {
    return WriteVar(L"BootNext", {reinterpret_cast<const std::uint8_t*>(&number), sizeof number});
}

Result<> ClearBootNext() { return WriteVar(L"BootNext", {}); }

Result<> MoveEntry(std::uint16_t number, std::size_t index) {
    auto existing = ReadEntry(number);
    if (!existing) return Fail(existing.error());
    if (!*existing) return Fail(Text::Format(IDS_ENTRY_MISSING, number));
    auto order = ReadU16List(L"BootOrder");
    if (!order) return Fail(order.error());
    std::erase(*order, number);
    index = std::min(index, order->size());
    order->insert(order->begin() + static_cast<std::ptrdiff_t>(index), number);
    return WriteU16List(L"BootOrder", *order);
}

Result<> MoveToFront(std::uint16_t number) { return MoveEntry(number, 0); }

Result<> RemoveFromOrder(std::uint16_t number) {
    auto order = ReadU16List(L"BootOrder");
    if (!order) return Fail(order.error());
    if (!std::erase(*order, number)) return {};
    if (order->empty()) return Fail(Text::Get(IDS_LAST_IN_ORDER));
    return WriteU16List(L"BootOrder", *order);
}

Result<> SetActive(std::uint16_t number, bool active) {
    auto var = ReadVar(BootName(number));
    if (!var) return Fail(var.error());
    if (!*var || (*var)->size() < 6) return Fail(Text::Format(IDS_ENTRY_MISSING, number));
    std::vector<std::uint8_t>& data = **var;
    std::uint32_t attributes = 0;
    std::memcpy(&attributes, data.data(), 4);
    const std::uint32_t wanted = active ? (attributes | kOptionActive) : (attributes & ~kOptionActive);
    if (wanted == attributes) return {};
    std::memcpy(data.data(), &wanted, 4);
    return WriteVar(BootName(number), data);
}

Result<> DeleteEntry(std::uint16_t number) {
    auto order = ReadU16List(L"BootOrder");
    if (!order) return Fail(order.error());
    if (std::erase(*order, number)) {
        if (auto w = WriteU16List(L"BootOrder", *order); !w) return Fail(w.error());
    }
    auto next = ReadU16List(L"BootNext");
    if (next && !next->empty() && next->front() == number) {
        if (auto w = ClearBootNext(); !w) return Fail(w.error());
    }
    return WriteVar(BootName(number), {});
}

}  // namespace DL::Efi
