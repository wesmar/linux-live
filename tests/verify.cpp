// Read-only verification against the real machine (no NVRAM/file writes).
// Build: pwsh tests\verify.ps1
#include "efi/Efi.cpp"       // include the .cpp to reach the anonymous-namespace helpers
#include "storage/Volumes.h"
#include "support/Config.h"
#include <cstdio>

using namespace DL;

int wmain() {
    Efi::EnablePrivilege(SE_SYSTEM_ENVIRONMENT_NAME).has_value();
    std::wprintf(L"UEFI: %d\n", Efi::IsUefi());

    auto esp = Storage::LocateEsp();
    if (!esp) { std::wprintf(L"ESP: FAIL %s\n", esp.error().c_str()); return 1; }
    std::wprintf(L"ESP: %s  root=%s  disk=%u part=%u start=%llu size=%llu gpt=%d\n", esp->volume.Name().c_str(),
                 esp->volume.root.c_str(), esp->partition.disk, esp->partition.number,
                 esp->partition.startLba, esp->partition.sizeLba, esp->partition.gpt);

    auto data = Storage::LocateData();
    if (!data) { std::wprintf(L"DATA: FAIL %s\n", data.error().c_str()); return 1; }
    std::wprintf(L"DATA: %s  dir=%s  exists=%d squashfs=%d\n", data->volume.Name().c_str(), data->dir.c_str(),
                 data->dirExists, data->hasSquashfs);

    auto entries = Efi::FindEntries(esp->partition, L"\\EFI\\linux");
    if (!entries) { std::wprintf(L"ENTRIES: FAIL %s\n", entries.error().c_str()); return 1; }
    int bad = 0;
    for (const auto& e : *entries) {
        auto raw = Efi::ReadVar(Efi::BootName(e.number));
        if (!raw || !*raw) { std::wprintf(L"Boot%04X: read fail\n", e.number); return 1; }
        const auto built = Efi::BuildLoadOption(esp->partition, e.filePath, e.description);
        const auto& real = **raw;
        // Attributes may legitimately differ (e.g. LOAD_OPTION_CATEGORY); compare the rest.
        const bool same = built.size() == real.size() && std::equal(built.begin() + 4, built.end(), real.begin() + 4);
        std::wprintf(L"Boot%04X '%s' %s  attr=%08X  built=%zu real=%zu  -> %s\n", e.number, e.description.c_str(),
                     e.filePath.c_str(), e.attributes, built.size(), real.size(), same ? L"IDENTICAL" : L"DIFFERENT");
        if (!same) {
            ++bad;
            for (std::size_t i = 0; i < std::max(built.size(), real.size()); ++i)
                if (i >= built.size() || i >= real.size() || built[i] != real[i]) {
                    std::wprintf(L"  first diff at byte %zu\n", i);
                    break;
                }
        }
    }
    std::wprintf(L"entries=%zu mismatches=%d\n", entries->size(), bad);
    return bad;
}
