// Windows' own copies of firmware boot entries, kept in the system BCD store.
//
// Windows mirrors NVRAM boot entries into BCD objects: an entry made with "bcdedit /copy {bootmgr}"
// (a Windows Boot Manager object pointing at another loader) or one Windows imported from NVRAM (a
// firmware application object). When the Boot#### variable of such an entry is missing, Windows
// writes it back from the BCD object at its next start - typically at the front of BootOrder. So
// deleting a boot entry for good means deleting its BCD copies as well. Read and changed through
// the BCD WMI provider (root\WMI, BcdStore / BcdObject), the documented interface behind bcdedit.
#pragma once
#include "efi/Efi.h"
#include <functional>
#include <string>
#include <vector>

namespace DL::Efi::Bcd {

struct Copy {
    std::wstring id;           // "{a608e44e-cd57-11f1-a746-9cb4f5d585ea}"
    std::wstring description;  // BcdLibraryString_Description
    std::wstring path;         // BcdLibraryString_ApplicationPath, e.g. \EFI\debian\shimx64.efi
};

struct Removal {
    std::vector<std::wstring> removed;  // ids of the BCD objects that were deleted
    std::wstring warning;               // last failure to delete one of them, if any
};

// Deletes the BCD objects that describe `entry`: the object named in its optional data (BCDOBJECT={...})
// and every firmware application / boot manager object with the same loader path and description.
// The Windows Boot Manager ({bootmgr}, \EFI\Microsoft\Boot\bootmgfw.efi) and {fwbootmgr} are never
// touched. `sharedElsewhere` (optional) is asked about a candidate that is not named by the entry and
// returns true when another remaining entry owns it; it is then kept. The store is opened once for
// the whole search and delete - opening it is the slow part.
[[nodiscard]] Result<Removal> RemoveCopies(const BootEntry& entry,
                                           const std::function<bool(const Copy&)>& sharedElsewhere = {});

}  // namespace DL::Efi::Bcd
