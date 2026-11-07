#include "core/Actions.h"
#include "efi/Bcd.h"
#include "install/EspInstaller.h"
#include "support/Config.h"
#include "support/Text.h"
#include <Windows.h>
#include <algorithm>

#pragma comment(lib, "advapi32.lib")

namespace DL::Core {

Result<> PrepareFirmwareAccess() {
    if (!Efi::IsUefi()) return Fail(Text::Get(IDS_NOT_UEFI));
    if (auto r = Efi::EnablePrivilege(SE_SYSTEM_ENVIRONMENT_NAME); !r)
        return Fail(Text::Format(IDS_NO_FIRMWARE_ACCESS, r.error()));
    (void)Efi::EnablePrivilege(SE_SHUTDOWN_NAME);  // only needed for a restart
    return {};
}

Result<Efi::EntryResult> EnsureLinuxEntry(const Storage::EspTarget& esp, std::wstring* loaderOut) {
    if (!esp.layout) return Fail(Text::Get(IDS_NOTHING_DEPLOYED));
    auto loader = Install::FindLoader(esp.dir, esp.layout->espFolder);
    if (!loader) return Fail(loader.error());
    if (loaderOut) *loaderOut = *loader;
    return Efi::EnsureEntry(esp.partition, *loader, Config::kEntryDescription);
}

Result<Deletion> DeleteEntry(const Efi::BootEntry& entry) {
    if (auto r = Efi::DeleteEntry(entry.number); !r) return Fail(r.error());
    Deletion result;
    // Another entry with the same description and loader may own the same copy; keep it then.
    // The NVRAM scan behind ListEntries is only paid for when a copy is actually found.
    bool loaded = false;
    std::vector<Efi::BootEntry> remaining;
    auto shared = [&](const Efi::Bcd::Copy& copy) {
        if (!loaded) {
            loaded = true;
            if (auto all = Efi::ListEntries()) remaining = std::move(*all);
        }
        return std::ranges::any_of(remaining, [&](const Efi::BootEntry& other) {
            return other.description == copy.description && _wcsicmp(other.filePath.c_str(), copy.path.c_str()) == 0;
        });
    };
    auto removed = Efi::Bcd::RemoveCopies(entry, shared);
    if (!removed) {
        result.bcdWarning = removed.error();
        return result;
    }
    result.bcdCopies = std::move(removed->removed);
    result.bcdWarning = std::move(removed->warning);
    return result;
}

Result<> Restart(bool force) {
    (void)Efi::EnablePrivilege(SE_SHUTDOWN_NAME);
    const DWORD flags = SHUTDOWN_RESTART | (force ? SHUTDOWN_FORCE_OTHERS : 0);
    const DWORD reason = SHTDN_REASON_MAJOR_OPERATINGSYSTEM | SHTDN_REASON_MINOR_RECONFIG | SHTDN_REASON_FLAG_PLANNED;
    if (const DWORD e = InitiateShutdownW(nullptr, nullptr, 0, flags, reason); e != ERROR_SUCCESS)
        return Fail(ErrorText(Text::Get(IDS_CTX_RESTART), e));
    return {};
}

}  // namespace DL::Core
