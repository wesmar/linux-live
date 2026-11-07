#include "storage/BootPartition.h"
#include "support/Config.h"
#include "support/Handle.h"
#include "support/Text.h"
#include "support/Wmi.h"
#include <Windows.h>
#include <winioctl.h>
#include <cwctype>
#include <format>
#include <thread>

namespace DL::Storage {
namespace {

using namespace Wmi;
using namespace std::chrono_literals;

constexpr wchar_t kNamespace[] = L"ROOT\\Microsoft\\Windows\\Storage";
constexpr wchar_t kBasicData[] = L"{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}";
constexpr GUID kBasicDataGuid = {0xebd0a0a2, 0xb9e5, 0x4433, {0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7}};
constexpr std::uint64_t kAlign = 1ull << 20;  // partitions start and end on 1 MiB boundaries
constexpr std::uint32_t kVolumeHasErrors = 42008;  // StorageWMI: "Cannot shrink a partition containing a volume with errors"

bool SameText(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) { return std::towlower(x) == std::towlower(y); });
}

// "\\?\Volume{GUID}\" -> "\\.\Volume{GUID}": the device itself, for DeviceIoControl.
std::wstring DevicePath(const std::wstring& root) {
    std::wstring path = root;
    while (!path.empty() && path.back() == L'\\') path.pop_back();
    if (path.starts_with(L"\\\\?\\")) path[2] = L'.';
    return path;
}

std::optional<PARTITION_INFORMATION_EX> PartitionInfo(HANDLE device) {
    PARTITION_INFORMATION_EX info{};
    DWORD bytes = 0;
    if (!DeviceIoControl(device, IOCTL_DISK_GET_PARTITION_INFO_EX, nullptr, 0, &info, sizeof info, &bytes, nullptr)) return std::nullopt;
    return info;
}

std::optional<std::uint32_t> DiskNumber(HANDLE device) {
    STORAGE_DEVICE_NUMBER number{};
    DWORD bytes = 0;
    if (!DeviceIoControl(device, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &number, sizeof number, &bytes, nullptr)) return std::nullopt;
    return number.DeviceNumber;
}

// Error text of a Storage Management API call: its extended status message when it has one.
std::wstring CallError(const wchar_t* method, IWbemClassObject* out) {
    const std::uint32_t code = ReturnValue(out);
    std::wstring message = GetString(GetEmbedded(out, L"ExtendedStatus").get(), L"Message");
    if (message.empty()) message = GetString(out, L"ExtendedStatus");
    return Text::Format(IDS_STORAGE_CALL_FAILED, method, message.empty() ? std::format(L"code {}", code) : message);
}

// One partition of the Storage Management API, re-read on demand (the objects are snapshots).
struct Part {
    std::wstring path;  // WMI object path for method calls
    std::uint32_t disk = 0, number = 0;
    std::uint64_t offset = 0, size = 0;
    std::vector<std::wstring> accessPaths;
};

Part ToPart(IWbemClassObject* o) {
    return {GetString(o, L"__PATH"),       GetU32(o, L"DiskNumber").value_or(0), GetU32(o, L"PartitionNumber").value_or(0),
            GetU64(o, L"Offset").value_or(0), GetU64(o, L"Size").value_or(0),   GetStrings(o, L"AccessPaths")};
}

class Storage {
public:
    static Result<Storage> Connect() {
        auto svc = Service::Connect(kNamespace);
        if (!svc) return Fail(svc.error());
        return Storage(std::move(*svc));
    }

    // The partition that holds the volume `root` (\\?\Volume{GUID}\).
    Result<Part> PartitionOf(const std::wstring& root) const {
        auto all = m_svc.Query(L"SELECT * FROM MSFT_Partition");
        if (!all) return Fail(all.error());
        for (const auto& o : *all) {
            Part p = ToPart(o.get());
            for (const auto& access : p.accessPaths)
                if (SameText(access, root)) return p;
        }
        return Fail(Text::Format(IDS_BOOTPART_NO_PARTITION, DisplayPath(root)));
    }

    Result<Part> Partition(std::uint32_t disk, std::uint32_t number) const {
        auto found = m_svc.Query(std::format(L"SELECT * FROM MSFT_Partition WHERE DiskNumber = {} AND PartitionNumber = {}", disk, number));
        if (!found) return Fail(found.error());
        if (found->empty()) return Fail(Text::Format(IDS_STORAGE_CALL_FAILED, L"MSFT_Partition", std::format(L"disk {} partition {}", disk, number)));
        return ToPart(found->front().get());
    }

    Result<std::uint16_t> PartitionStyle(std::uint32_t disk, std::wstring* path = nullptr) const {
        auto found = m_svc.Query(std::format(L"SELECT * FROM MSFT_Disk WHERE Number = {}", disk));
        if (!found) return Fail(found.error());
        if (found->empty()) return Fail(Text::Format(IDS_STORAGE_CALL_FAILED, L"MSFT_Disk", std::format(L"disk {}", disk)));
        if (path) *path = GetString(found->front().get(), L"__PATH");
        return static_cast<std::uint16_t>(GetU32(found->front().get(), L"PartitionStyle").value_or(0));
    }

    struct Range {
        std::uint64_t min = 0, max = 0;
    };
    Result<Range> SupportedSize(const Part& p, std::uint32_t* code = nullptr) const {
        auto out = m_svc.Call(p.path, L"GetSupportedSize", nullptr);
        if (!out) return Fail(out.error());
        if (code) *code = ReturnValue(out->get());
        if (ReturnValue(out->get()) != 0) return Fail(CallError(L"GetSupportedSize", out->get()));
        return Range{GetU64(out->get(), L"SizeMin").value_or(0), GetU64(out->get(), L"SizeMax").value_or(0)};
    }

    Result<> Resize(const Part& p, std::uint64_t size, std::uint32_t* code = nullptr) const {
        auto in = m_svc.InParams(L"MSFT_Partition", L"Resize");
        if (!in) return Fail(in.error());
        PutU64(in->get(), L"Size", size);
        auto out = m_svc.Call(p.path, L"Resize", in->get());
        if (!out) return Fail(out.error());
        if (code) *code = ReturnValue(out->get());
        if (ReturnValue(out->get()) != 0) return Fail(CallError(L"Resize", out->get()));
        return {};
    }

    Result<Part> Create(const std::wstring& diskPath, std::uint64_t offset, std::uint64_t size) const {
        auto in = m_svc.InParams(L"MSFT_Disk", L"CreatePartition");
        if (!in) return Fail(in.error());
        PutU64(in->get(), L"Offset", offset);
        PutU64(in->get(), L"Size", size);
        PutString(in->get(), L"GptType", kBasicData);
        PutBool(in->get(), L"AssignDriveLetter", false);
        auto out = m_svc.Call(diskPath, L"CreatePartition", in->get());
        if (!out) return Fail(out.error());
        if (ReturnValue(out->get()) != 0) return Fail(CallError(L"CreatePartition", out->get()));
        const Object created = GetEmbedded(out->get(), L"CreatedPartition");
        if (!created) return Fail(Text::Format(IDS_STORAGE_CALL_FAILED, L"CreatePartition", L"no partition returned"));
        return Partition(GetU32(created.get(), L"DiskNumber").value_or(0), GetU32(created.get(), L"PartitionNumber").value_or(0));
    }

    Result<> Format(const std::wstring& volumeRoot) const {
        auto volumes = m_svc.Query(L"SELECT * FROM MSFT_Volume");
        if (!volumes) return Fail(volumes.error());
        for (const auto& v : *volumes) {
            if (!SameText(GetString(v.get(), L"Path"), volumeRoot)) continue;
            auto in = m_svc.InParams(L"MSFT_Volume", L"Format");
            if (!in) return Fail(in.error());
            PutString(in->get(), L"FileSystem", L"FAT32");
            PutString(in->get(), L"FileSystemLabel", Config::kBootPartitionLabel);
            PutBool(in->get(), L"Full", false);
            PutBool(in->get(), L"Force", true);
            auto out = m_svc.Call(GetString(v.get(), L"__PATH"), L"Format", in->get());
            if (!out) return Fail(out.error());
            if (ReturnValue(out->get()) != 0) return Fail(CallError(L"Format", out->get()));
            return {};
        }
        return Fail(Text::Get(IDS_BOOTPART_NO_VOLUME));
    }

    Result<> Delete(const Part& p) const {
        auto out = m_svc.Call(p.path, L"DeleteObject", nullptr);
        if (!out) return Fail(out.error());
        if (ReturnValue(out->get()) != 0) return Fail(CallError(L"DeleteObject", out->get()));
        return {};
    }

private:
    explicit Storage(Service svc) : m_svc(std::move(svc)) {}
    Service m_svc;
};

// A new partition gets its volume (\\?\Volume{GUID}\) once the mount manager has seen it.
std::wstring WaitForVolume(const Storage& storage, const Part& p) {
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (auto fresh = storage.Partition(p.disk, p.number))
            for (const auto& access : fresh->accessPaths)
                if (access.starts_with(L"\\\\?\\Volume{")) return access;
        std::this_thread::sleep_for(250ms);
    }
    return {};
}

// GPT name (how FindBootPartition recognises it) and "no drive letter" (Windows keeps it out of
// Explorer and never assigns a letter, also on another installation).
Result<> MarkPartition(const std::wstring& volumeRoot) {
    FileHandle device(CreateFileW(DevicePath(volumeRoot).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, 0, nullptr));
    if (!device) return Fail(ErrorText(Text::Get(IDS_BOOTPART_MARK_FAILED), GetLastError()));
    const auto info = PartitionInfo(device.get());
    if (!info || info->PartitionStyle != PARTITION_STYLE_GPT) return Fail(Text::Get(IDS_BOOTPART_MARK_FAILED));
    SET_PARTITION_INFORMATION_EX set{};
    set.PartitionStyle = PARTITION_STYLE_GPT;
    set.Gpt = info->Gpt;
    set.Gpt.Attributes |= GPT_BASIC_DATA_ATTRIBUTE_NO_DRIVE_LETTER;
    wcsncpy_s(set.Gpt.Name, Config::kBootPartitionName, _TRUNCATE);
    DWORD bytes = 0;
    if (!DeviceIoControl(device.get(), IOCTL_DISK_SET_PARTITION_INFO_EX, &set, sizeof set, nullptr, 0, &bytes, nullptr))
        return Fail(ErrorText(Text::Get(IDS_BOOTPART_MARK_FAILED), GetLastError()));
    return {};
}

std::uint64_t FreeBytes(const std::wstring& root) {
    ULARGE_INTEGER free{};
    return GetDiskFreeSpaceExW(root.c_str(), &free, nullptr, nullptr) ? free.QuadPart : 0;
}

// The NTFS dirty bit: set while another system has the volume mounted for writing and not cleared
// when it was not shut down cleanly. Windows does not shrink such a volume before chkdsk ran.
bool VolumeDirty(const std::wstring& root) {
    FileHandle device(CreateFileW(DevicePath(root).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
    ULONG flags = 0;
    DWORD bytes = 0;
    return device && DeviceIoControl(device.get(), FSCTL_IS_VOLUME_DIRTY, nullptr, 0, &flags, sizeof flags, &bytes, nullptr) &&
           (flags & VOLUME_IS_DIRTY) != 0;
}

}  // namespace

std::optional<BootPartition> FindBootPartition() {
    for (const auto& volume : EnumerateVolumes()) {
        if (!volume.fileSystem.starts_with(L"FAT")) continue;
        FileHandle device(CreateFileW(DevicePath(volume.root).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
        if (!device) continue;
        const auto info = PartitionInfo(device.get());
        if (!info || info->PartitionStyle != PARTITION_STYLE_GPT || !IsEqualGUID(info->Gpt.PartitionType, kBasicDataGuid) ||
            std::wstring_view(info->Gpt.Name) != Config::kBootPartitionName)
            continue;
        BootPartition result;
        result.volume = volume;
        result.disk = DiskNumber(device.get()).value_or(0);
        result.number = info->PartitionNumber;
        result.offset = static_cast<std::uint64_t>(info->StartingOffset.QuadPart);
        result.size = static_cast<std::uint64_t>(info->PartitionLength.QuadPart);
        result.freeBytes = FreeBytes(volume.root);
        return result;
    }
    return std::nullopt;
}

Result<BootPartition> CreateBootPartition(const Volume& windows, std::uint64_t size, const LogFn& log) {
    ComScope com;
    size = (size + kAlign - 1) / kAlign * kAlign;
    auto storage = Storage::Connect();
    if (!storage) return Fail(storage.error());
    auto part = storage->PartitionOf(windows.root);
    if (!part) return Fail(part.error());
    std::wstring diskPath;
    auto style = storage->PartitionStyle(part->disk, &diskPath);
    if (!style) return Fail(style.error());
    if (*style != 2) return Fail(Text::Format(IDS_BOOTPART_NOT_GPT, windows.Name()));  // 2 = GPT

    // Windows does not shrink a volume whose file system needs repair (for example after another
    // system wrote to it and was not shut down cleanly); chkdsk /f at the next restart fixes that.
    if (VolumeDirty(windows.root)) return Fail(Text::Format(IDS_BOOTPART_VOLUME_ERRORS, windows.Name()));
    std::uint32_t code = 0;
    auto range = storage->SupportedSize(*part, &code);
    if (!range) return Fail(code == kVolumeHasErrors ? Text::Format(IDS_BOOTPART_VOLUME_ERRORS, windows.Name()) : range.error());
    const std::uint64_t original = part->size;
    const std::uint64_t shrunk = (original - size) / kAlign * kAlign;
    if (original <= size || shrunk < range->min)
        return Fail(Text::Format(IDS_BOOTPART_CANNOT_SHRINK, windows.Name(), FormatBytes(size), FormatBytes(original - range->min)));

    log(Text::Format(IDS_BOOTPART_SHRINKING, windows.Name(), FormatBytes(original - shrunk)));
    if (auto r = storage->Resize(*part, shrunk, &code); !r)
        return Fail(code == kVolumeHasErrors ? Text::Format(IDS_BOOTPART_VOLUME_ERRORS, windows.Name()) : r.error());

    // From here on a failure gives the space back to Windows.
    auto rollback = [&](const Part* created, std::wstring error) -> Result<BootPartition> {
        if (created) (void)storage->Delete(*created);
        if (auto fresh = storage->PartitionOf(windows.root); fresh && storage->Resize(*fresh, original))
            log(Text::Format(IDS_BOOTPART_ROLLBACK, windows.Name()));
        return Fail(std::move(error));
    };
    auto created = storage->Create(diskPath, part->offset + shrunk, size);
    if (!created) return rollback(nullptr, created.error());
    const std::wstring root = WaitForVolume(*storage, *created);
    if (root.empty()) return rollback(&*created, Text::Get(IDS_BOOTPART_NO_VOLUME));
    if (auto r = storage->Format(root); !r) return rollback(&*created, r.error());
    if (auto r = MarkPartition(root); !r) return rollback(&*created, r.error());

    BootPartition result;
    result.volume.root = root;
    result.volume.fileSystem = L"FAT32";
    result.volume.size = size;
    result.disk = created->disk;
    result.number = created->number;
    result.offset = created->offset;
    result.size = created->size;
    result.freeBytes = FreeBytes(root);
    log(Text::Format(IDS_BOOTPART_CREATED, FormatBytes(result.size), result.disk, result.number));
    return result;
}

Result<> RemoveBootPartition(const BootPartition& partition, const Volume& windows, const LogFn& log) {
    ComScope com;
    auto storage = Storage::Connect();
    if (!storage) return Fail(storage.error());
    // Windows grows a volume that needs repair no more than it shrinks one: check before deleting,
    // so the partition is never gone while its space cannot be given back.
    if (VolumeDirty(windows.root)) return Fail(Text::Format(IDS_BOOTPART_REMOVE_DIRTY, windows.Name()));
    auto part = storage->Partition(partition.disk, partition.number);
    if (!part) return Fail(part.error());
    auto win = storage->PartitionOf(windows.root);
    const bool adjacent = win && win->disk == part->disk && win->offset + win->size == part->offset;
    if (auto r = storage->Delete(*part); !r) return Fail(r.error());
    if (!adjacent) {
        log(Text::Format(IDS_BOOTPART_REMOVED_ONLY, windows.Name()));
        return {};
    }
    auto fresh = storage->PartitionOf(windows.root);
    if (!fresh) return Fail(fresh.error());
    auto range = storage->SupportedSize(*fresh);
    if (!range) return Fail(range.error());
    if (range->max > fresh->size)
        if (auto r = storage->Resize(*fresh, range->max); !r) return Fail(r.error());
    log(Text::Format(IDS_BOOTPART_REMOVED, windows.Name(), FormatBytes(range->max - std::min(range->max, fresh->size)),
                     FormatBytes(std::max(range->max, fresh->size))));
    return {};
}

}  // namespace DL::Storage
