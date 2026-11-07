// Where a live package puts its files. Nothing here is a constant of this program: the package
// decides, so any distribution with a signed shim + GRUB brings its own layout.
//
//   ESP folder   = the prefix built into the signed GRUB (Debian /EFI/debian, Fedora /EFI/fedora ...).
//                  It is part of the signed image and cannot be chosen; GRUB reads grub.cfg there.
//   media folder = live-media-path in grub.cfg (usually through "set media=..."), relative to the
//                  root of the Windows volume, where filesystem.squashfs lives.
#pragma once
#include "support/Common.h"
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace DL::Install {

struct Layout {
    std::wstring espFolder;    // relative to the ESP root, backslashes: "EFI\debian"
    std::wstring mediaFolder;  // relative to the NTFS root, backslashes: "linux-live"
    bool kernelSearch = false;  // grub.cfg looks for the kernel on other partitions ("search --file"),
                                // so kernel and initrd may live on a boot partition (storage/BootPartition.h)
    bool operator==(const Layout&) const = default;
};

// Layout of an ESP package (esp.tar): reads grubx64.efi and grub.cfg out of the archive.
[[nodiscard]] Result<Layout> ReadLayout(const std::filesystem::path& tarFile);

// Layout of the package deployed on an ESP: the EFI\<folder> whose grub.cfg boots a live system.
[[nodiscard]] std::optional<Layout> FindDeployed(const std::filesystem::path& espRoot);

// True when <ESP>\<folder> holds the boot loader of an installed system: a grub.cfg that does not
// start a live system. Deploying there would replace that system's GRUB configuration.
[[nodiscard]] bool BelongsToInstalledSystem(const std::filesystem::path& espFolder);

// True when grub.cfg starts a live system (live-boot or dracut live), i.e. it is ours to replace.
[[nodiscard]] bool IsLiveConfig(std::string_view grubCfg);

// True when grub.cfg searches other partitions for a file ("search ... --file"), the fallback that
// lets the kernel and initrd live outside the ESP.
[[nodiscard]] bool SearchesKernel(std::string_view grubCfg);

// Parsers, public for the tests.
// Prefix from the "mods" section of a GRUB EFI image, as "EFI\debian"; nullopt if absent or unusable.
[[nodiscard]] std::optional<std::wstring> GrubPrefix(std::span<const std::uint8_t> efiImage);
// live-media-path from grub.cfg with $variables resolved, as "linux-live"; "live" when not given
// (the live-boot default); nullopt when the value is unusable (absolute, "..", empty).
[[nodiscard]] std::optional<std::wstring> MediaFolder(std::string_view grubCfg);

}  // namespace DL::Install
