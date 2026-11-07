#include "storage/Volumes.h"
#include "support/Config.h"
#include "support/Text.h"
#include <Windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <algorithm>
#include <cwctype>

namespace DL::Storage {
namespace fs = std::filesystem;

std::wstring Volume::Name() const {
    if (mount.empty()) return root;
    std::wstring name = mount;
    while (name.size() > 1 && name.back() == L'\\') name.pop_back();
    return name;
}

std::wstring DisplayPath(const fs::path& path) {
    std::wstring text = path.wstring();
    constexpr std::wstring_view kPrefix = L"\\\\?\\Volume{";
    if (!text.starts_with(kPrefix)) return text;
    const std::size_t close = text.find(L"}\\", kPrefix.size());
    if (close == std::wstring::npos) return text;
    const std::wstring volume = text.substr(0, close + 2);
    DWORD length = 0;
    GetVolumePathNamesForVolumeNameW(volume.c_str(), nullptr, 0, &length);
    if (length <= 1) return text;
    std::wstring names(length, L'\0');
    if (!GetVolumePathNamesForVolumeNameW(volume.c_str(), names.data(), length, &length) || names[0] == L'\0') return text;
    return names.c_str() + text.substr(close + 2);  // the first name, e.g. "C:\", then the rest
}

Result<> ProtectMediaFolder(const fs::path& dir) {
    // P = protected (no inheritance from C:\), AI = auto-inherited children; OICI = files and folders.
    constexpr wchar_t kSddl[] = L"D:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)";
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kSddl, SDDL_REVISION_1, &sd, nullptr))
        return Fail(ErrorText(Text::Format(IDS_PROTECT_FOLDER_FAILED, DisplayPath(dir)), GetLastError()));
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = nullptr;
    GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted);
    std::wstring path = dir.wstring();
    const DWORD error = SetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                              nullptr, nullptr, dacl, nullptr);
    LocalFree(sd);
    if (error != ERROR_SUCCESS) return Fail(ErrorText(Text::Format(IDS_PROTECT_FOLDER_FAILED, DisplayPath(dir)), error));
    return {};
}

std::vector<Volume> EnumerateVolumes() {
    std::vector<Volume> volumes;
    wchar_t name[MAX_PATH]{};
    HANDLE find = FindFirstVolumeW(name, MAX_PATH);
    if (find == INVALID_HANDLE_VALUE) return volumes;
    do {
        Volume v;
        v.root = name;
        wchar_t fsName[64]{};
        if (GetVolumeInformationW(name, nullptr, 0, nullptr, nullptr, nullptr, fsName, 64)) v.fileSystem = fsName;
        DWORD length = 0;
        GetVolumePathNamesForVolumeNameW(name, nullptr, 0, &length);
        if (length > 1) {
            std::wstring paths(length, L'\0');
            if (GetVolumePathNamesForVolumeNameW(name, paths.data(), length, &length)) v.mount = paths.c_str();
        }
        ULARGE_INTEGER total{};
        if (GetDiskFreeSpaceExW(name, nullptr, &total, nullptr)) v.size = total.QuadPart;
        volumes.push_back(std::move(v));
    } while (FindNextVolumeW(find, name, MAX_PATH));
    FindVolumeClose(find);
    return volumes;
}

namespace {

std::optional<Volume> SystemVolume(const std::vector<Volume>& volumes) {
    wchar_t windows[MAX_PATH]{};
    if (!GetSystemWindowsDirectoryW(windows, MAX_PATH)) return std::nullopt;
    wchar_t root[MAX_PATH]{};
    if (!GetVolumePathNameW(windows, root, MAX_PATH)) return std::nullopt;
    wchar_t guid[MAX_PATH]{};
    if (!GetVolumeNameForVolumeMountPointW(root, guid, MAX_PATH)) return std::nullopt;
    for (const auto& v : volumes)
        if (_wcsicmp(v.root.c_str(), guid) == 0) return v;
    return std::nullopt;
}

bool IsFat(const std::wstring& name) { return name.empty() || name == L"FAT32" || name == L"FAT"; }

}  // namespace

Result<EspTarget> LocateEsp() {
    const auto volumes = EnumerateVolumes();
    std::vector<std::pair<Volume, Efi::Partition>> esps;
    for (const auto& v : volumes) {
        if (!IsFat(v.fileSystem)) continue;
        if (auto p = Efi::QueryPartition(v.root); p && p->isEsp) esps.emplace_back(v, *p);
    }
    if (esps.empty()) return Fail(Text::Get(IDS_NO_ESP));

    std::size_t pick = esps.size();
    std::vector<std::optional<Install::Layout>> layouts;
    for (const auto& e : esps) layouts.push_back(Install::FindDeployed(fs::path(e.first.root)));
    if (esps.size() == 1) {
        pick = 0;
    } else {
        // 1) the ESP that already holds a live package
        for (std::size_t i = 0; i < esps.size() && pick == esps.size(); ++i)
            if (layouts[i]) pick = i;
        // 2) the ESP on the Windows system disk
        if (pick == esps.size()) {
            if (auto sys = SystemVolume(volumes)) {
                if (auto sp = Efi::QueryPartition(sys->root)) {
                    for (std::size_t i = 0; i < esps.size(); ++i)
                        if (esps[i].second.disk == sp->disk) { pick = i; break; }
                }
            }
        }
    }
    if (pick == esps.size()) pick = 0;

    EspTarget target;
    target.volume = esps[pick].first;
    target.partition = esps[pick].second;
    if (layouts[pick]) UseLayout(target, *layouts[pick]);
    return target;
}

void UseLayout(EspTarget& esp, const Install::Layout& layout) {
    esp.layout = layout;
    esp.dir = fs::path(esp.volume.root) / layout.espFolder;
}

Result<DataTarget> LocateData(const Install::Layout* layout) {
    const auto volumes = EnumerateVolumes();
    std::error_code ec;
    const Volume* withFolder = nullptr;
    const Volume* withSquashfs = nullptr;
    for (const auto& v : volumes) {
        if (v.fileSystem != L"NTFS" || !layout) continue;
        const fs::path dir = fs::path(v.root) / layout->mediaFolder;
        if (!fs::is_directory(dir, ec)) continue;
        if (!withFolder) withFolder = &v;
        if (!withSquashfs && fs::is_regular_file(dir / Config::kSquashfsName, ec)) withSquashfs = &v;
    }
    const Volume* chosen = withSquashfs ? withSquashfs : withFolder;
    std::optional<Volume> system;
    if (!chosen) {
        system = SystemVolume(volumes);
        if (!system || system->fileSystem != L"NTFS") return Fail(Text::Get(IDS_NO_NTFS));
        chosen = &*system;
    }
    DataTarget target;
    target.volume = *chosen;
    if (!layout) return target;  // nothing deployed yet: the volume is known, the folder is not
    target.dir = fs::path(chosen->root) / layout->mediaFolder;
    target.dirExists = fs::is_directory(target.dir, ec);
    target.hasSquashfs = fs::is_regular_file(target.dir / Config::kSquashfsName, ec);
    return target;
}

}  // namespace DL::Storage


