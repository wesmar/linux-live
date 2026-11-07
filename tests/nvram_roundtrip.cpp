// NVRAM round trip against the real firmware: create, reorder, BootNext, disable/enable, delete.
// Every change is undone; BootOrder and BootNext must end byte for byte as they started (they are
// restored even when a step fails). Needs administrator rights and UEFI; Secure Boot may be on.
//
//   nvram_roundtrip.exe            run the test (exit code 0 = passed)
//   nvram_roundtrip.exe save F     write BootOrder / BootNext to F
//   nvram_roundtrip.exe restore F  write them back from F
#include "efi/Efi.cpp"  // the anonymous-namespace helpers (ReadVar / WriteVar) are needed for raw access
#include "storage/Volumes.h"
#include <cstdio>
#include <fstream>
#include <iterator>

using namespace DL;

namespace {

struct Snapshot {
    std::vector<std::uint8_t> order;
    std::optional<std::vector<std::uint8_t>> next;  // nullopt = BootNext absent
};

Result<Snapshot> Take() {
    auto order = Efi::ReadVar(L"BootOrder");
    if (!order || !*order) return Fail(L"BootOrder unreadable");
    auto next = Efi::ReadVar(L"BootNext");
    if (!next) return Fail(next.error());
    return Snapshot{**order, *next};
}

Result<> Put(const Snapshot& s) {
    if (auto r = Efi::WriteVar(L"BootOrder", s.order); !r) return r;
    if (s.next) return Efi::WriteVar(L"BootNext", *s.next);
    auto current = Efi::ReadVar(L"BootNext");
    if (current && *current) return Efi::WriteVar(L"BootNext", {});
    return {};
}

int g_failures = 0;
void Check(bool ok, const wchar_t* what) {
    std::wprintf(L"  [%s] %s\n", ok ? L" OK " : L"FAIL", what);
    if (!ok) ++g_failures;
}

std::optional<std::size_t> PositionOf(std::uint16_t number) {
    auto order = Efi::ReadBootOrder();
    if (!order) return std::nullopt;
    for (std::size_t i = 0; i < order->size(); ++i)
        if ((*order)[i] == number) return i;
    return std::nullopt;
}

int RunTest() {
    auto before = Take();
    if (!before) { std::wprintf(L"cannot read NVRAM: %s\n", before.error().c_str()); return 2; }
    auto esp = Storage::LocateEsp();
    if (!esp) { std::wprintf(L"no ESP: %s\n", esp.error().c_str()); return 2; }
    std::wprintf(L"BootOrder has %zu entries; Secure Boot %s\n", before->order.size() / 2,
                 Efi::SecureBootEnabled().value_or(false) ? L"on" : L"off/unknown");

    // A loader path that cannot collide with a real entry; the file does not need to exist.
    const std::wstring path = L"\\EFI\\linux\\nvram-roundtrip-test.efi";
    std::uint16_t number = 0;
    {
        auto created = Efi::EnsureEntry(esp->partition, path, L"Linux Live NVRAM test");
        Check(created && created->created, L"create a new Boot#### entry");
        if (!created) { std::wprintf(L"    %s\n", created.error().c_str()); return 1; }
        number = created->number;
        std::wprintf(L"    Boot%04X\n", number);
    }
    auto entry = Efi::ReadEntry(number);
    Check(entry && *entry && (*entry)->description == L"Linux Live NVRAM test" && (*entry)->filePath == path &&
              Efi::OnPartition(**entry, esp->partition) && (*entry)->Active(),
          L"read it back: description, path, ESP partition GUID, active");
    const std::size_t count = before->order.size() / 2 + 1;
    Check(PositionOf(number) == count - 1, L"appended to the end of BootOrder (default unchanged)");
    auto again = Efi::EnsureEntry(esp->partition, path, L"other text");
    Check(again && !again->created && again->number == number, L"an equal entry is reused, not duplicated");

    Check(Efi::MoveEntry(number, count - 2).has_value() && PositionOf(number) == count - 2, L"move up one position");
    Check(Efi::MoveEntry(number, 0).has_value() && PositionOf(number) == 0, L"move to the front (make default)");
    Check(Efi::MoveEntry(number, 1).has_value() && PositionOf(number) == 1, L"move down one position");
    Check(Efi::RemoveFromOrder(number).has_value() && !PositionOf(number), L"remove from BootOrder, entry kept");
    Check(Efi::ReadEntry(number).value_or(std::nullopt).has_value(), L"the variable still exists");
    Check(Efi::MoveEntry(number, 3).has_value() && PositionOf(number) == 3, L"insert an unlisted entry at position 4");

    Check(Efi::SetActive(number, false).has_value() && !Efi::ReadEntry(number)->value().Active(), L"disable (LOAD_OPTION_ACTIVE off)");
    Check(Efi::SetActive(number, true).has_value() && Efi::ReadEntry(number)->value().Active(), L"enable again");

    Check(Efi::SetBootNext(number).has_value() && Efi::BootNext() == number, L"BootNext set");
    Check(Efi::DeleteEntry(number).has_value(), L"delete the entry");
    Check(!Efi::ReadEntry(number).value_or(std::nullopt).has_value(), L"the variable is gone");
    Check(!PositionOf(number), L"no BootOrder reference left");
    Check(Efi::BootNext() != number, L"BootNext reference cleared with it");

    // Undo whatever is left (also after failures) and compare raw bytes.
    (void)Efi::DeleteEntry(number);
    if (auto r = Put(*before); !r) std::wprintf(L"restore failed: %s\n", r.error().c_str());
    auto after = Take();
    Check(after && after->order == before->order, L"BootOrder identical to the start, byte for byte");
    Check(after && after->next == before->next, L"BootNext identical to the start");
    std::wprintf(g_failures ? L"FAILED: %d check(s)\n" : L"PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (auto r = Efi::EnablePrivilege(SE_SYSTEM_ENVIRONMENT_NAME); !r) {
        std::wprintf(L"%s\n", r.error().c_str());
        return 2;
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"save") {
        auto s = Take();
        if (!s) return 2;
        std::ofstream out(argv[2], std::ios::binary);
        const std::uint32_t n = static_cast<std::uint32_t>(s->order.size()), hasNext = s->next ? 1u : 0u;
        out.write(reinterpret_cast<const char*>(&n), 4).write(reinterpret_cast<const char*>(s->order.data()), n);
        out.write(reinterpret_cast<const char*>(&hasNext), 4);
        if (s->next) {
            const std::uint32_t m = static_cast<std::uint32_t>(s->next->size());
            out.write(reinterpret_cast<const char*>(&m), 4).write(reinterpret_cast<const char*>(s->next->data()), m);
        }
        std::wprintf(L"saved %u bytes of BootOrder%s\n", n, s->next ? L" + BootNext" : L"");
        return out ? 0 : 1;
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"restore") {
        std::ifstream in(argv[2], std::ios::binary);
        Snapshot s;
        std::uint32_t n = 0, hasNext = 0;
        in.read(reinterpret_cast<char*>(&n), 4);
        if (!in || n == 0 || n > 4096 || n % 2) return 2;
        s.order.resize(n);
        in.read(reinterpret_cast<char*>(s.order.data()), n).read(reinterpret_cast<char*>(&hasNext), 4);
        if (hasNext) {
            std::uint32_t m = 0;
            in.read(reinterpret_cast<char*>(&m), 4);
            if (m > 64) return 2;
            s.next.emplace(m);
            in.read(reinterpret_cast<char*>(s.next->data()), m);
        }
        if (!in) return 2;
        auto r = Put(s);
        std::wprintf(r ? L"restored\n" : L"restore failed: %s\n", r ? L"" : r.error().c_str());
        return r ? 0 : 1;
    }
    return RunTest();
}
