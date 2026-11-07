// Boot entry list as shown by the GUI and `list` on the command line.
#pragma once
#include "efi/Efi.h"
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace DL::Boot {

struct Row {
    Efi::BootEntry entry;
    int position = -1;     // index in BootOrder, -1 = not listed
    bool current = false;  // BootCurrent
    bool next = false;     // BootNext
    bool linux = false;    // below the ESP folder of the deployed package (e.g. \EFI\debian)
};

struct Snapshot {
    std::vector<Row> rows;  // BootOrder first, then unlisted entries by number
    std::size_t ordered = 0;
    std::size_t hiddenApps = 0;  // firmware applications left out (includeApps == false)
    std::optional<std::uint16_t> current, next;

    const Row* Find(std::uint16_t number) const;
    const Row* Linux() const;  // first Linux row (prefers one in BootOrder)
};

// `esp` may be null (no ESP found): rows are then recognised by path only. `espFolder` is the ESP
// folder of the deployed package ("EFI\debian"); empty = nothing deployed, no row is ours.
Result<Snapshot> Read(const Efi::Partition* esp, std::wstring_view espFolder, bool includeApps);

// "0006", "Boot0006", "6", "0x6" -> 0x0006 (Boot#### numbers are hexadecimal).
std::optional<std::uint16_t> ParseNumber(std::wstring_view text);

std::wstring Name(std::uint16_t number);      // "Boot0006"
std::wstring Target(const Efi::BootEntry& e);  // "HD(1,GPT) \EFI\debian\shimx64.efi" or "NVMe"

}  // namespace DL::Boot
