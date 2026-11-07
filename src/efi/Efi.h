// UEFI NVRAM boot-entry API + ESP partition discovery (Windows, requires administrator).
// Boot#### / BootOrder / BootNext are ordinary (not authenticated) variables, so all of this works
// with Secure Boot enabled; Secure Boot only governs which loaders the firmware accepts.
#pragma once
#include "support/Common.h"
#include <Windows.h>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace DL::Efi {

inline constexpr std::uint32_t kOptionActive = 0x00000001;
inline constexpr std::uint32_t kOptionHidden = 0x00000008;
inline constexpr std::uint32_t kOptionCategoryMask = 0x00001F00;
inline constexpr std::uint32_t kOptionCategoryApp = 0x00000100;

// Location of a partition on a physical disk (needed to build an EFI HD() device path).
struct Partition {
    std::uint32_t disk = 0;
    std::uint32_t number = 0;
    std::uint64_t startLba = 0;
    std::uint64_t sizeLba = 0;
    std::uint64_t sizeBytes = 0;
    bool gpt = false;
    bool isEsp = false;
    GUID id{};                       // GPT partition GUID
    std::uint32_t mbrSignature = 0;  // MBR disk signature
};

struct BootEntry {
    std::uint16_t number = 0;       // Boot#### number
    std::uint32_t attributes = 0;
    std::wstring description;
    std::wstring filePath;          // e.g. \EFI\linux\shimx64.efi
    std::wstring device;            // short device-path summary: "HD(1,GPT)", "NVMe", "Firmware app" ...
    bool hasPartitionGuid = false;  // device path contains HD() with a GPT GUID
    GUID partitionGuid{};
    bool hasMbrSignature = false;   // device path contains HD() with an MBR signature
    std::uint32_t mbrSignature = 0;
    std::uint32_t partitionNumber = 0;  // HD() partition number
    bool inOrder = false;
    std::size_t orderPosition = 0;  // valid when inOrder
    std::wstring bcdObject;         // "{guid}" when Windows wrote the entry from its BCD store (see Bcd.h)

    bool Active() const { return (attributes & kOptionActive) != 0; }
    bool Hidden() const { return (attributes & kOptionHidden) != 0; }
    bool Application() const { return (attributes & kOptionCategoryMask) == kOptionCategoryApp; }
};

// --- Environment -----------------------------------------------------------
bool IsUefi();
Result<> EnablePrivilege(const wchar_t* name);       // e.g. SE_SYSTEM_ENVIRONMENT_NAME
std::optional<bool> SecureBootEnabled();             // nullopt when unknown

// --- ESP discovery ---------------------------------------------------------
// `volumeRoot` is a volume GUID path such as \\?\Volume{...}\ (no drive letter needed).
Result<Partition> QueryPartition(const std::wstring& volumeRoot);

// --- NVRAM boot entries ----------------------------------------------------
// Entries in BootOrder first (in that order), then every other Boot#### variable found.
Result<std::vector<BootEntry>> ListEntries();
Result<std::optional<BootEntry>> ReadEntry(std::uint16_t number);
std::optional<std::uint16_t> BootCurrent();
std::optional<std::uint16_t> BootNext();

Result<std::vector<std::uint16_t>> ReadBootOrder();
Result<> WriteBootOrder(const std::vector<std::uint16_t>& order);

// True when the entry boots from `esp`.
bool OnPartition(const BootEntry& e, const Partition& esp);

// Entries that boot from `esp` and live under `directory` (e.g. \EFI\linux), case-insensitive.
Result<std::vector<BootEntry>> FindEntries(const Partition& esp, std::wstring_view directory);
// Same filter over an already read list (avoids re-reading NVRAM).
std::vector<BootEntry> FilterEntries(const std::vector<BootEntry>& all, const Partition& esp,
                                     std::wstring_view directory);

struct EntryResult {
    std::uint16_t number = 0;
    bool created = false;  // false = an equivalent entry already existed and was left untouched
};

// Ensures a Boot#### entry for esp:loaderPath exists. An existing entry (same partition and
// same path) is never rewritten. A new or unlisted entry is appended to BootOrder WITHOUT
// changing the default.
Result<EntryResult> EnsureEntry(const Partition& esp, std::wstring_view loaderPath,
                                std::wstring_view description);

// One-time boot on next restart.
Result<> SetBootNext(std::uint16_t number);
Result<> ClearBootNext();

// Moves the entry to `index` in BootOrder (clamped; an unlisted entry is inserted there).
Result<> MoveEntry(std::uint16_t number, std::size_t index);
// Permanent default: the first position in BootOrder.
Result<> MoveToFront(std::uint16_t number);
// Takes the entry out of BootOrder but keeps the Boot#### variable.
Result<> RemoveFromOrder(std::uint16_t number);

// Sets or clears LOAD_OPTION_ACTIVE (the rest of the variable is rewritten byte for byte).
Result<> SetActive(std::uint16_t number, bool active);

// Removes the variable and its BootOrder / BootNext references.
Result<> DeleteEntry(std::uint16_t number);

}  // namespace DL::Efi
