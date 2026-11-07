// Project-wide constants. No drive letters: volumes are located through the Windows storage API.
#pragma once
#include <array>
#include <cstdint>
#include <string_view>

namespace DL::Config {

inline constexpr wchar_t kAppTitle[] = L"Linux Live";
inline constexpr wchar_t kAppVersion[] = L"1.1.0";
inline constexpr wchar_t kCopyright[] = L"\u00A9 2026 Marek Weso\u0142owski - wesmar";
inline constexpr wchar_t kExeName[] = L"LinuxLive.exe";
inline constexpr wchar_t kRegKey[] = L"Software\\KVC\\LinuxLive";

// Default source: the latest GitHub release of this repository. "releases/latest/download/<asset>"
// always resolves to the newest release, so the URLs never change between versions.
inline constexpr wchar_t kGitHubRepo[] = L"wesmar/linux-live";
inline constexpr wchar_t kImageAsset[] = L"filesystem.squashfs";
inline constexpr wchar_t kEspAsset[] = L"esp.tar";

// Test locations used by earlier builds; only read to migrate old settings into "Custom".
inline constexpr wchar_t kLegacyImageUrl[] = L"https://kvc.pl/filesystem.squashfs";
inline constexpr wchar_t kLegacyEspUrl[] = L"https://kvc.pl/esp.tar";

// The folders are not constants: a package states them (install/Layout.h) - the ESP folder is the
// prefix of its signed GRUB, the folder on the Windows volume is live-media-path in its grub.cfg.
inline constexpr wchar_t kSquashfsName[] = L"filesystem.squashfs";
inline constexpr wchar_t kPackageName[] = L"esp.tar";

// ESP package (esp.tar, uncompressed: shimx64.efi, grubx64.efi, grub.cfg, vmlinuz, initrd.img,
// unicode.pf2) is extracted into <ESP>\<prefix of its signed GRUB>. Uncompressed on purpose: kernel
// and initrd are already compressed, so a compressor would only cost time.
// Space that must stay free on the ESP after deploying: firmware (BIOS/UEFI) updates stage their
// capsules there, and a laptop BIOS image alone is 20-40 MB. 20 % of the ESP, never less than 20 MB:
// 20 MB on an old 100 MB ESP, 40 MB on the 200 MB ESP of current Windows installations.
inline constexpr std::uint64_t kEspReserveMinBytes = 20ull << 20;
inline constexpr std::uint64_t kEspReservePercent = 20;
constexpr std::uint64_t EspReserve(std::uint64_t espSize) {
    const std::uint64_t share = espSize / 100 * kEspReservePercent;
    return share > kEspReserveMinBytes ? share : kEspReserveMinBytes;
}

// Boot partition for ESPs too small for the kernel and initrd (storage/BootPartition.h): cut from the
// end of the Windows volume on request, FAT32, no drive letter, found again by its GPT name.
inline constexpr std::uint64_t kBootPartitionBytes = 200ull << 20;
inline constexpr wchar_t kBootPartitionName[] = L"Linux Live boot";  // GPT partition name (36 characters at most)
inline constexpr wchar_t kBootPartitionLabel[] = L"LINUXLIVE";       // FAT32 volume label (11 characters at most)

// Integrity check: before a download is accepted its SHA-256 is compared with "<url>.sha256"
// (plain hex digest, "sha256sum" format is fine). false = a missing .sha256 only logs a warning;
// true = a download without a checksum is refused.
inline constexpr bool kRequireSha256 = false;

// Description for a newly created NVRAM entry (an existing entry is left untouched).
inline constexpr wchar_t kEntryDescription[] = L"Linux";

// EFI loaders tried in order (the first existing file becomes the boot target).
inline constexpr std::array<std::wstring_view, 2> kLoaderCandidates{L"shimx64.efi", L"grubx64.efi"};

}  // namespace DL::Config
