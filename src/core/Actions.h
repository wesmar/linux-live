// Operations shared by the GUI and the command line.
#pragma once
#include "efi/Efi.h"
#include "storage/Volumes.h"
#include <string>
#include <vector>

namespace DL::Core {

// Enables the privileges NVRAM access and restarting need. Fails without administrator rights.
[[nodiscard]] Result<> PrepareFirmwareAccess();

// Creates the NVRAM entry for the loader of the deployed package (<ESP>\<its folder>), or reuses an equal one.
[[nodiscard]] Result<Efi::EntryResult> EnsureLinuxEntry(const Storage::EspTarget& esp, std::wstring* loader = nullptr);

struct Deletion {
    std::vector<std::wstring> bcdCopies;  // Windows' BCD copies of the entry that were removed as well
    std::wstring bcdWarning;              // set when those copies could not be checked or removed
};

// Deletes a boot entry for good: the Boot#### variable (and its BootOrder / BootNext references),
// then the copies Windows keeps of it in its BCD store, which would otherwise bring it back at the
// next Windows start (see efi/Bcd.h). A copy shared with another remaining entry is left alone.
[[nodiscard]] Result<Deletion> DeleteEntry(const Efi::BootEntry& entry);

// Restarts the computer now (works from an ssh session as well). `force` closes other users' sessions.
[[nodiscard]] Result<> Restart(bool force);

}  // namespace DL::Core
