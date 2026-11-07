// A small FAT32 partition for the kernel and initrd, cut from the end of the Windows volume, for ESPs
// that are too small to hold them (installations older than Windows 11 24H2 often have a 100 MB ESP,
// most of it taken by Windows).
//
// Only the signed shim, GRUB and grub.cfg stay on the ESP: GRUB has to start from the ESP folder its
// signature names. When vmlinuz is not next to it, grub.cfg finds it on this partition through
// "search --file /<media>/vmlinuz", and shim still checks the kernel's signature there. The free space
// that firmware updates need on the ESP is left untouched (Config::EspReserve).
//
// Created and removed with the Windows Storage Management API (MSFT_Partition / MSFT_Disk /
// MSFT_Volume), the interface Disk Management, diskpart and Resize-Partition use: it moves NTFS
// data out of the way, keeps the partition table consistent and works on BitLocker volumes.
// Removing the partition returns the space to the Windows volume. The partition is recognised by its
// GPT partition name, so the registry is not needed to find it again.
#pragma once
#include "storage/Volumes.h"
#include <cstdint>
#include <optional>

namespace DL::Storage {

struct BootPartition {
    Volume volume;  // \\?\Volume{GUID}\ - no drive letter is assigned
    std::uint32_t disk = 0;
    std::uint32_t number = 0;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::uint64_t freeBytes = 0;
};

// The partition this program created, if it exists.
[[nodiscard]] std::optional<BootPartition> FindBootPartition();

// Shrinks `windows` by `size` bytes and creates the FAT32 boot partition in the freed space directly
// after it. The Windows volume is grown back if a later step fails.
[[nodiscard]] Result<BootPartition> CreateBootPartition(const Volume& windows, std::uint64_t size, const LogFn& log);

// Deletes the boot partition and gives its space back to `windows` when the two are adjacent.
[[nodiscard]] Result<> RemoveBootPartition(const BootPartition& partition, const Volume& windows, const LogFn& log);

}  // namespace DL::Storage
