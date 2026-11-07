// Volume discovery through the Windows storage API (volume GUID paths, no drive letters).
#pragma once
#include "efi/Efi.h"
#include "install/Layout.h"
#include <filesystem>
#include <optional>
#include <vector>

namespace DL::Storage {

struct Volume {
    std::wstring root;        // \\?\Volume{GUID}\  (always usable for file access as administrator)
    std::wstring mount;       // first mount point, e.g. "C:\" (may be empty)
    std::wstring fileSystem;  // "NTFS", "FAT32", ... (empty when unreadable)
    std::uint64_t size = 0;
    std::wstring Name() const;  // "C:" or the GUID path
};

std::vector<Volume> EnumerateVolumes();

// A path for messages: \\?\Volume{GUID}\x is shown as C:\x when that volume has a drive letter or
// mount point; any other path is returned as it is.
std::wstring DisplayPath(const std::filesystem::path& path);

struct EspTarget {
    Volume volume;
    Efi::Partition partition;
    std::optional<Install::Layout> layout;  // package deployed on this ESP, if any
    std::filesystem::path dir;              // <ESP>\<layout->espFolder>; empty while nothing is deployed
};

// Points `esp` at the ESP folder of `layout` (after a deployment or for a package about to be deployed).
void UseLayout(EspTarget& esp, const Install::Layout& layout);

struct DataTarget {
    Volume volume;
    std::filesystem::path dir;  // <NTFS>\<layout.mediaFolder>; empty without a layout
    bool dirExists = false;
    bool hasSquashfs = false;
};

// ESP choice: the one holding a deployed live package, otherwise the ESP on the Windows system
// disk, otherwise the first ESP found. The deployed layout is read from it (Install::FindDeployed).
Result<EspTarget> LocateEsp();

// The media folder decides what Linux runs as root: the image, the persistence layer, authorized_keys
// and the cached package. Under C:\ every authenticated user may modify new folders, so its DACL is
// set explicitly - full control for SYSTEM and Administrators, read and execute for Users, nothing
// inherited from the volume root. Files already inside take the new inheritable entries.
Result<> ProtectMediaFolder(const std::filesystem::path& dir);

// NTFS choice for `layout`: the volume that already holds its media folder (preferably with the
// squashfs), otherwise the Windows system volume. Without a layout: the system volume, no folder.
Result<DataTarget> LocateData(const Install::Layout* layout);

}  // namespace DL::Storage
