// ESP deployment: extracts the esp.tar package into <ESP>\<folder of the signed GRUB> (see Layout.h).
#pragma once
#include "support/Common.h"
#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string_view>
#include <vector>

namespace DL::Install {

struct Report {
    std::wstring loaderPath;  // e.g. \EFI\debian\shimx64.efi
    std::uint64_t bytes = 0;
    unsigned files = 0;
    unsigned skipped = 0;
    bool split = false;  // kernel and initrd went to the boot partition
};

// Finds the EFI loader present in `espDir` (= <ESP>\<espFolder>), returned as an EFI path
// (backslashes, rooted): "\EFI\debian\shimx64.efi".
Result<std::wstring> FindLoader(const std::filesystem::path& espDir, std::wstring_view espFolder);

// Contents of one file of the package, found by name at the archive root or below it
// (case-insensitive, first match). Refuses files larger than `limit`.
Result<std::vector<std::uint8_t>> ReadPackageFile(const std::filesystem::path& tarFile, std::wstring_view name,
                                                  std::uint64_t limit);

struct Destination {
    std::wstring volumeRoot;     // volume of `dir`, for the free-space check
    std::filesystem::path dir;   // target folder
    std::uint64_t reserve = 0;   // bytes that must stay free on that volume afterwards
};

// Files that must sit next to the signed GRUB: EFI binaries, their CSV files and grub.cfg. The rest
// (kernel, initrd, font) can be loaded from another FAT partition (see storage/BootPartition.h).
bool LoaderFile(const std::filesystem::path& name);

// Space the package needs on the ESP, by group, before anything is written.
struct EspPlan {
    std::uint64_t loaderGrowth = 0;  // bytes the loader files add on the ESP
    std::uint64_t kernelGrowth = 0;  // bytes the kernel files add on the ESP
    std::uint64_t kernelBytes = 0;   // size of the kernel files
    std::uint64_t kernelOnEsp = 0;   // size of their copies already on the ESP (freed when they move away)
    std::uint64_t freeBytes = 0;
    std::uint64_t reserve = 0;
    bool FitsAll() const { return loaderGrowth + kernelGrowth + reserve <= freeBytes; }
    bool FitsLoader() const { return loaderGrowth + reserve <= freeBytes + kernelOnEsp; }
};
Result<EspPlan> PlanEsp(const std::filesystem::path& tarFile, const Destination& esp, std::wstring_view espFolder);

// Extracts an (uncompressed) ustar/pax/GNU tar.
//  * the whole archive is validated first (paths, types, free space) - nothing is written on error;
//  * entries prefixed with the ESP folder ("EFI/debian/") are accepted too (prefix is stripped);
//  * `esp.reserve` stays free on the ESP afterwards (firmware update capsules);
//  * with `kernel`, everything but the loader files goes there, and copies of those files left on
//    the ESP by an earlier deployment are deleted, so GRUB falls back to the other partition;
//  * files are written to "<name>.tmp", flushed, then renamed; identical files (size + time) are skipped.
Result<Report> DeployEsp(const std::filesystem::path& tarFile, const Destination& esp, const Destination* kernel,
                         std::wstring_view espFolder, const ProgressFn& progress, const LogFn& log, std::stop_token stop);

}  // namespace DL::Install
