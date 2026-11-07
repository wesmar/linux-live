#include "app/Cli.h"
#include "app/Console.h"
#include "core/Actions.h"
#include "core/BootList.h"
#include "core/Deploy.h"
#include "core/Sources.h"
#include "install/EspInstaller.h"
#include "storage/BitLocker.h"
#include "storage/BootPartition.h"
#include "support/Config.h"
#include "support/Text.h"
#include <Windows.h>
#include <algorithm>
#include <cwctype>
#include <format>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace DL::Cli {
namespace {

// ---------------------------------------------------------------------------
// Arguments
// ---------------------------------------------------------------------------
std::wstring Lower(std::wstring_view s) {
    std::wstring out(s);
    for (auto& c : out) c = static_cast<wchar_t>(std::towlower(c));
    return out;
}

std::wstring Join(const std::vector<std::wstring>& items, std::wstring_view separator = L", ") {
    std::wstring out;
    for (const auto& item : items) {
        if (!out.empty()) out += separator;
        out += item;
    }
    return out;
}

bool IsHelp(std::wstring_view arg) {
    static constexpr std::wstring_view kHelp[] = {L"help", L"--help", L"-help", L"-h", L"-?", L"/?", L"/h", L"/help", L"--usage"};
    return std::ranges::contains(kHelp, Lower(arg));
}

enum Option : unsigned {
    kAll = 1u << 0, kYes = 1u << 1, kForce = 1u << 2, kGitHub = 1u << 3, kBitLockerKey = 1u << 4,
    kNoProgress = 1u << 5, kClear = 1u << 6, kImage = 1u << 7, kEsp = 1u << 8, kNoEntry = 1u << 9,
    kBootPartition = 1u << 10,
};

struct OptionSpec {
    std::wstring_view name;
    Option bit;
    bool value;
};
constexpr OptionSpec kOptions[] = {
    {L"--all", kAll, false},           {L"--yes", kYes, false},       {L"--force", kForce, false},
    {L"--github", kGitHub, false},     {L"--bitlocker-key", kBitLockerKey, false},
    {L"--no-progress", kNoProgress, false}, {L"--clear", kClear, false},
    {L"--image", kImage, true},        {L"--esp", kEsp, true},
    {L"--no-entry", kNoEntry, false},  {L"--boot-partition", kBootPartition, false},
};

struct CommandSpec {
    std::wstring_view name;
    unsigned options;     // allowed options
    std::size_t minArgs, maxArgs;
};
constexpr CommandSpec kCommands[] = {
    {L"status", 0, 0, 0},
    {L"list", kAll, 0, 0},
    {L"deploy", kImage | kEsp | kGitHub | kBitLockerKey | kNoProgress | kNoEntry | kBootPartition, 0, 0},
    {L"boot-partition", kYes, 0, 1},
    {L"source", kImage | kEsp, 0, 1},
    {L"add", 0, 0, 0},
    {L"once", kClear, 0, 1},
    {L"default", 0, 0, 1},
    {L"up", 0, 1, 1},
    {L"down", 0, 1, 1},
    {L"move", 0, 2, 2},
    {L"unlist", 0, 1, 1},
    {L"enable", 0, 1, 1},
    {L"disable", 0, 1, 1},
    {L"delete", kYes | kForce, 1, 1},
    {L"reboot", kYes | kForce, 0, 0},
    {L"version", 0, 0, 0},
    {L"lng-export", 0, 1, 1},
    {L"lng-check", 0, 0, 1},
};

struct Args {
    const CommandSpec* command = nullptr;
    std::vector<std::wstring> positional;
    unsigned options = 0;
    std::wstring image, esp;
    bool Has(Option o) const { return (options & o) != 0; }
};

struct UsageError {
    std::wstring message;
};

std::optional<UsageError> Parse(std::span<wchar_t*> argv, Args& args) {
    std::vector<std::wstring_view> words(argv.begin(), argv.end());
    for (std::size_t i = 0; i < words.size(); ++i) {
        const std::wstring word(words[i]);
        if (word.starts_with(L"--") || (word.size() > 1 && word[0] == L'-' && !std::iswdigit(word[1]))) {
            std::wstring name = Lower(word), value;
            const std::size_t eq = name.find(L'=');
            bool inlineValue = false;
            if (eq != std::wstring::npos) {
                value = word.substr(eq + 1);
                name.resize(eq);
                inlineValue = true;
            }
            if (name == L"--version") {
                if (!args.command) args.command = &kCommands[std::size(kCommands) - 1];
                continue;
            }
            const auto spec = std::ranges::find(kOptions, name, &OptionSpec::name);
            if (spec == std::end(kOptions)) return UsageError{std::format(L"Unknown option '{}'.", word)};
            if (spec->value && !inlineValue) {
                if (i + 1 >= words.size()) return UsageError{std::format(L"Option '{}' needs a value.", spec->name)};
                value = words[++i];
            } else if (!spec->value && inlineValue) {
                return UsageError{std::format(L"Option '{}' takes no value.", spec->name)};
            }
            args.options |= spec->bit;
            if (spec->bit == kImage) args.image = value;
            if (spec->bit == kEsp) args.esp = value;
            continue;
        }
        if (!args.command) {
            const auto spec = std::ranges::find(kCommands, Lower(word), &CommandSpec::name);
            if (spec == std::end(kCommands)) return UsageError{std::format(L"Unknown command '{}'.", word)};
            args.command = &*spec;
            continue;
        }
        args.positional.push_back(word);
    }
    if (!args.command) return UsageError{L"No command given."};
    const CommandSpec& c = *args.command;
    if (const unsigned extra = args.options & ~c.options) {
        for (const auto& o : kOptions)
            if (extra & o.bit) return UsageError{std::format(L"Option '{}' does not apply to '{}'.", o.name, c.name)};
    }
    if (args.positional.size() < c.minArgs) return UsageError{std::format(L"'{}' needs more arguments.", c.name)};
    if (args.positional.size() > c.maxArgs)
        return UsageError{std::format(L"Unexpected argument '{}' for '{}'.", args.positional[c.maxArgs], c.name)};
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Help
// ---------------------------------------------------------------------------
std::wstring HelpTextImpl() {
    return std::format(LR"({0} {1} - boots a Linux live system stored on the Windows NTFS volume
through the signed shim, with Secure Boot left enabled.

USAGE
  {2}                       start the graphical interface
  {2} <command> [options]   command line (any argument selects it)

INFORMATION
  status                    Firmware, Secure Boot, BitLocker, data volume, ESP, source
  list [--all]              NVRAM boot entries: BootOrder first, then unlisted entries
                            --all  also firmware applications and hidden entries

DEPLOYMENT
  deploy                    Download (or copy) the image and the ESP package, install the ESP
    --image <url|file>      image source for this run only
    --esp <url|file>        ESP package source for this run only
    --github                GitHub release for this run, whatever is saved
    --bitlocker-key         if the data volume is BitLocker-encrypted, store its recovery
                            password on the ESP (unencrypted!) so Linux can unlock it
    --no-entry              do not create the Linux NVRAM entry afterwards
    --boot-partition        if the ESP is too small for kernel and initrd (old 100 MB ESPs), shrink
                            the Windows volume by 200 MB and put them on a FAT32 partition of their
                            own; the signed boot loader and the firmware update reserve stay on the ESP
    --no-progress           no progress bars
  boot-partition            Show the boot partition (only made for an ESP that is too small)
  boot-partition remove --yes
                            Delete it and give its space back to the Windows volume
  source                    Show the saved source
  source github             Save: GitHub release of {3} (default)
  source custom [--image <url|file>] [--esp <url|file>]
                            Save: own locations (https:// URL or local file)

BOOT ENTRIES     <entry> = Boot#### number (hexadecimal): 0006, Boot0006, 6 - or "linux"
  add                       Create the Linux entry (an equal one is reused), add it to BootOrder
  once [<entry>]            Boot the entry once at the next restart (BootNext), default: linux
  once --clear              Clear BootNext
  default [<entry>]         Move the entry to the front of BootOrder, default: linux
  up <entry>                Move one position up in BootOrder
  down <entry>              Move one position down
  move <entry> <position>   Move to <position> (1 = first)
  unlist <entry>            Take the entry out of BootOrder, keep it in NVRAM
  enable <entry>            Mark the entry active
  disable <entry>           Mark the entry inactive (the firmware skips it)
  delete <entry> --yes      Delete the entry from NVRAM
                            --force  as well for the entry this system was booted from
  reboot --yes              Restart now (--force also signs out other users)

  lng-export <file>         Write the English texts as a template for language\<code>.lng
                            (translate it, keep the numbers; read by the graphical interface)
  lng-check [<file>]        Check language\*.lng (or one file): coverage, unknown ids, broken {{}}
                            fields, the Windows languages each file covers and the automatic choice
  help | --help | -h | -? | /?    This help         version | --version    Version

EXIT CODES
  0 success  1 failed  2 invalid arguments  3 not possible here (no administrator rights,
  no UEFI, no ESP)  4 canceled (Ctrl+C)

NOTES
  Run from an elevated terminal; an administrator ssh session already is one.
  Nothing is asked interactively: confirmations are options, so scripts and ssh behave
  the same as a terminal. cmd, batch files and "ssh host command" wait for the program;
  interactive PowerShell does not - use  {2} list | Out-Host  when the exit code matters.
)",
                       Config::kAppTitle, Config::kAppVersion, Config::kExeName, Config::kGitHubRepo);
}

void PrintHelp(Console& con) { con.Out(HelpTextImpl()); }

int UsageFailure(Console& con, std::wstring_view message) {
    con.Status(Level::Error, std::format(L"{} Use --help for usage.", message));
    return kUsage;
}

// ---------------------------------------------------------------------------
// Ctrl+C
// ---------------------------------------------------------------------------
std::stop_source* g_stop = nullptr;

BOOL WINAPI OnConsoleCtrl(DWORD type) {
    if ((type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) && g_stop) {
        g_stop->request_stop();
        return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
class Runner {
public:
    Runner(Console& con, const Args& args) : m_con(con), m_args(args) {}

    int Run() {
        const std::wstring_view cmd = m_args.command->name;
        if (cmd == L"version") {
            m_con.Line(std::format(L"{} {}", Config::kAppTitle, Config::kAppVersion));
            return kOk;
        }
        if (cmd == L"source" && m_args.positional.empty() && !m_args.options) return ShowSource();
        if (cmd == L"lng-check") return CheckLanguages();
        if (cmd == L"lng-export") {
            if (!Text::ExportTemplate(m_args.positional[0])) return Failure(std::format(L"Cannot write {}.", m_args.positional[0]));
            m_con.Status(Level::Success, std::format(L"English template written to {}.", m_args.positional[0]));
            return kOk;
        }
        if (!IsElevated()) {
            m_con.Status(Level::Error, L"Administrator rights are required. Run from an elevated terminal "
                                       L"(an administrator ssh session is elevated already).");
            return kUnavailable;
        }
        if (cmd == L"source") return SetSource();
        if (cmd == L"deploy") return Deploy();
        if (cmd == L"boot-partition") return BootPartitionCommand();
        if (cmd == L"status") return Status();
        if (cmd == L"reboot") return Reboot();

        if (auto r = Core::PrepareFirmwareAccess(); !r) {
            m_con.Status(Level::Error, r.error());
            return kUnavailable;
        }
        if (cmd == L"list") return List();
        if (cmd == L"add") return Add();
        return Modify(cmd);
    }

private:
    int Failure(const std::wstring& message, int code = kFailed) {
        m_con.Status(Level::Error, message);
        return code;
    }

    // ESP is optional for most NVRAM commands: without it Linux entries are matched by path only.
    void LoadEsp() {
        if (m_espLoaded) return;
        m_espLoaded = true;
        if (auto esp = Storage::LocateEsp()) m_esp = *esp;
        else m_espError = esp.error();
    }

    Result<Boot::Snapshot> Snapshot(bool includeApps = false) {
        LoadEsp();
        return Boot::Read(m_esp ? &m_esp->partition : nullptr, EspFolder(), includeApps);
    }

    // ESP folder of the deployed package ("EFI\\debian"); empty while nothing is deployed.
    std::wstring EspFolder() const { return m_esp && m_esp->layout ? m_esp->layout->espFolder : std::wstring(); }

    static std::wstring Shown(const Storage::Volume& v, const std::filesystem::path& dir, std::wstring_view folder) {
        return v.mount.empty() ? dir.wstring() : v.Name() + L"\\" + std::wstring(folder);
    }

    // <entry>: "linux" or a hexadecimal number of an existing entry.
    Result<std::uint16_t> ResolveEntry(std::wstring_view text, const Boot::Snapshot& snap) {
        if (Lower(text) == L"linux") {
            const Boot::Row* row = snap.Linux();
            if (!row) return Fail(L"There is no Linux entry. Create it with 'add' (after 'deploy').");
            return row->entry.number;
        }
        const auto number = Boot::ParseNumber(text);
        if (!number) return Fail(std::format(L"'{}' is not a Boot#### number (hexadecimal, e.g. 0006).", text));
        auto entry = Efi::ReadEntry(*number);
        if (!entry) return Fail(entry.error());
        if (!*entry) return Fail(std::format(L"{} does not exist.", Boot::Name(*number)));
        return *number;
    }

    std::wstring Describe(std::uint16_t number, const Boot::Snapshot& snap) {
        const Boot::Row* row = snap.Find(number);
        return row ? std::format(L"{} \"{}\"", Boot::Name(number), row->entry.description) : Boot::Name(number);
    }

    // --- language files ----------------------------------------------------------
    int CheckLanguages() {
        const std::wstring one = m_args.positional.empty() ? std::wstring() : m_args.positional[0];
        const auto reports = Text::Check(one);
        if (reports.empty()) {
            if (!one.empty()) return Failure(std::format(L"Cannot read {}.", one));
            m_con.Status(Level::Info, L"No language files (language\\*.lng next to the executable): English only.");
            return kOk;
        }
        bool problems = false;
        for (const auto& r : reports) {
            std::wstring locales;
            for (const auto& l : r.language.locales) locales += (locales.empty() ? L"" : L",") + l;
            const bool clean = r.missing.empty() && r.unknown.empty() && r.placeholders.empty();
            problems = problems || !clean;
            m_con.Status(clean ? Level::Success : r.placeholders.empty() ? Level::Warning : Level::Error,
                         std::format(L"{}  {}  {}/{} strings ({}%)  locale {}", std::filesystem::path(r.file).filename().wstring(),
                                     r.language.name, r.language.translated, r.language.total, r.language.Coverage(),
                                     locales.empty() ? std::wstring(L"by file name") : locales));
            auto list = [&](const wchar_t* what, const std::vector<UINT>& ids) {
                if (ids.empty()) return;
                std::wstring text;
                for (std::size_t i = 0; i < ids.size() && i < 32; ++i) text += std::format(L" {}", ids[i]);
                if (ids.size() > 32) text += L" ...";
                m_con.Line(std::format(L"         {} ({}):{}", what, ids.size(), text));
            };
            list(L"missing", r.missing);
            list(L"unknown ids", r.unknown);
            list(L"broken {} fields - shown in English", r.placeholders);
        }
        m_con.Line(std::format(L"Automatic choice for this Windows user: {}", Text::Detect()));
        return problems ? kFailed : kOk;
    }

    // --- status ----------------------------------------------------------------
    int ShowSource() {
        const auto mode = Sources::LoadMode();
        const auto active = Sources::Active();
        m_con.Line(std::format(L"Source  {}", mode == Sources::Mode::GitHub
                                                  ? std::format(L"GitHub release of {} (default)", Config::kGitHubRepo)
                                                  : std::wstring(L"custom")));
        m_con.Line(std::format(L"  image  {}", active.image));
        m_con.Line(std::format(L"  esp    {}", active.esp));
        return kOk;
    }

    int SetSource() {
        if (m_args.positional.empty()) return UsageFailure(m_con, L"Use 'source github' or 'source custom ...'.");
        const std::wstring which = Lower(m_args.positional[0]);
        if (which == L"github") {
            if (m_args.Has(kImage) || m_args.Has(kEsp)) return UsageFailure(m_con, L"'source github' takes no --image / --esp.");
            Sources::SaveMode(Sources::Mode::GitHub);
        } else if (which == L"custom") {
            Sources::Pair custom = Sources::LoadCustom();
            if (m_args.Has(kImage)) custom.image = m_args.image;
            if (m_args.Has(kEsp)) custom.esp = m_args.esp;
            if (custom.image.empty() || custom.esp.empty())
                return UsageFailure(m_con, L"A custom source needs both --image and --esp (once; they are remembered).");
            if (auto r = Sources::Check(custom.image, Sources::Part::Image); !r) return Failure(r.error(), kUsage);
            if (auto r = Sources::Check(custom.esp, Sources::Part::Package); !r) return Failure(r.error(), kUsage);
            Sources::SaveCustom(custom);
            Sources::SaveMode(Sources::Mode::Custom);
        } else {
            return UsageFailure(m_con, std::format(L"Unknown source '{}' (github or custom).", m_args.positional[0]));
        }
        m_con.Status(Level::Success, L"Source saved.");
        return ShowSource();
    }

    int Status() {
        const bool uefi = Efi::IsUefi();
        const bool nvram = uefi && Core::PrepareFirmwareAccess().has_value();
        std::wstring firmware = uefi ? L"UEFI" : L"legacy BIOS (no NVRAM boot entries)";
        if (nvram) {
            const auto sb = Efi::SecureBootEnabled();
            firmware += std::format(L", Secure Boot {}", sb ? (*sb ? L"on" : L"off") : L"unknown");
        }
        m_con.Line(std::format(L"{} {}", Config::kAppTitle, Config::kAppVersion));
        m_con.Line(std::format(L"  Firmware     {}", firmware));

        LoadEsp();
        const Install::Layout* layout = m_esp && m_esp->layout ? &*m_esp->layout : nullptr;
        if (layout) m_con.Line(std::format(L"  Layout       ESP folder {} (signed GRUB), media folder {} (grub.cfg)", layout->espFolder,
                                           layout->mediaFolder));
        else m_con.Line(L"  Layout       nothing deployed yet - the package decides at 'deploy'");
        if (auto data = Storage::LocateData(layout)) {
            std::error_code ec;
            const auto image = data->dir / Config::kSquashfsName;
            const std::wstring where = layout ? Shown(data->volume, data->dir, layout->mediaFolder) : data->volume.Name();
            const std::wstring state = !layout              ? std::wstring(L"not deployed")
                                       : data->hasSquashfs ? std::format(L"image {}", FormatBytes(std::filesystem::file_size(image, ec)))
                                                           : std::wstring(L"no image yet");
            m_con.Line(std::format(L"  Data         {}  ({}, {})", where, data->volume.fileSystem, state));
            if (auto bl = Storage::QueryBitLocker(data->volume.root)) {
                using S = Storage::BitLockerStatus::State;
                const wchar_t* text = bl->state == S::NotEncrypted ? L"not encrypted"
                                      : bl->state == S::Encrypted  ? (bl->protectionOn ? L"encrypted, protection on" : L"encrypted, protection suspended")
                                                                   : L"unknown";
                m_con.Line(std::format(L"  BitLocker    {}", text));
            }
        } else {
            m_con.Line(std::format(L"  Data         {}", data.error()));
        }

        if (m_esp) {
            const auto& p = m_esp->partition;
            auto loader = layout ? Install::FindLoader(m_esp->dir, layout->espFolder) : Result<std::wstring>(Fail(std::wstring()));
            const std::wstring where = layout ? Shown(m_esp->volume, m_esp->dir, layout->espFolder) : m_esp->volume.Name();
            m_con.Line(std::format(L"  ESP          {}  (disk {}, partition {}, {}, {}, loader {})", where, p.disk, p.number,
                                   p.gpt ? L"GPT" : L"MBR", FormatBytes(p.sizeBytes),
                                   loader ? loader->substr(loader->rfind(L'\\') + 1) : std::wstring(L"none - not deployed")));
        } else {
            m_con.Line(std::format(L"  ESP          {}", m_espError));
        }
        if (const auto boot = Storage::FindBootPartition())
            m_con.Line(std::format(L"  Boot part.   FAT32 {}, disk {}, partition {}, {} free - {}", FormatBytes(boot->size), boot->disk,
                                   boot->number, FormatBytes(boot->freeBytes),
                                   KernelOnBootPartition(*boot) ? L"holds kernel and initrd" : L"not used (the ESP holds the kernel)"));

        const auto active = Sources::Active();
        m_con.Line(std::format(L"  Source       {}", Sources::LoadMode() == Sources::Mode::GitHub ? L"GitHub release" : L"custom"));
        m_con.Line(std::format(L"    image      {}", active.image));
        m_con.Line(std::format(L"    esp        {}", active.esp));

        if (nvram) {
            if (auto snap = Snapshot()) {
                if (const Boot::Row* row = snap->Linux())
                    m_con.Line(std::format(L"  Boot entry   {}  {}", Describe(row->entry.number, *snap),
                                           row->position >= 0 ? std::format(L"position {} of {}", row->position + 1, snap->ordered)
                                                              : std::wstring(L"not in BootOrder")));
                else
                    m_con.Line(L"  Boot entry   none (run 'add' after 'deploy')");
                m_con.Line(std::format(L"  BootNext     {}", snap->next ? Describe(*snap->next, *snap) : std::wstring(L"not set")));
            }
        }
        return kOk;
    }

    // --- list ------------------------------------------------------------------
    int List() {
        auto snap = Snapshot(m_args.Has(kAll));
        if (!snap) return Failure(snap.error());
        std::size_t wDesc = 11;
        for (const auto& r : snap->rows) wDesc = std::max(wDesc, std::min<std::size_t>(r.entry.description.size(), 36));
        const bool vt = m_con.color();
        auto paint = [vt](std::wstring_view code, const std::wstring& text) {
            return vt ? std::format(L"\x1b[{}m{}\x1b[0m", code, text) : text;
        };

        m_con.Line(std::format(L" {:>3}  {:<8}  {:<15}  {:<{}}  {}", L"Pos", L"Entry", L"Flags", L"Description", wDesc, L"Target"));
        bool separator = false;
        for (const auto& r : snap->rows) {
            if (r.position < 0 && !separator) {
                separator = true;
                m_con.Line(paint(L"90", L"  -- not in BootOrder --"));
            }
            std::vector<std::wstring> flags;
            if (r.current) flags.emplace_back(L"current");
            if (r.next) flags.emplace_back(L"next");
            if (!r.entry.Active()) flags.emplace_back(L"off");
            if (r.entry.Hidden()) flags.emplace_back(L"hidden");
            if (r.entry.Application()) flags.emplace_back(L"app");
            if (r.linux) flags.emplace_back(L"linux");
            std::wstring flagText;
            for (const auto& f : flags) flagText += (flagText.empty() ? L"" : L",") + f;
            std::wstring desc = r.entry.description;
            if (desc.size() > 36) desc = desc.substr(0, 35) + L"\x2026";

            const std::wstring pos = r.position >= 0 ? std::to_wstring(r.position + 1) : L"-";
            std::wstring line = std::format(L" {:>3}  {:<8}  {:<15}  {:<{}}  {}", pos, Boot::Name(r.entry.number), flagText, desc,
                                            wDesc, Boot::Target(r.entry));
            if (vt && !r.entry.Active()) line = paint(L"90", line);
            else if (vt && r.current) line = paint(L"32", line);
            else if (vt && r.linux) line = paint(L"38;2;235;80;130", line);
            m_con.Line(line);
        }
        const auto sb = Efi::SecureBootEnabled();
        std::wstring summary = std::format(L"BootCurrent {}   BootNext {}   Secure Boot {}",
                                           snap->current ? Boot::Name(*snap->current) : std::wstring(L"?"),
                                           snap->next ? Boot::Name(*snap->next) : std::wstring(L"not set"),
                                           sb ? (*sb ? L"on" : L"off") : L"unknown");
        if (snap->hiddenApps) summary += std::format(L"   ({} firmware applications hidden, --all shows them)", snap->hiddenApps);
        m_con.Line();
        m_con.Line(summary);
        return kOk;
    }

    // --- add ---------------------------------------------------------------------
    int Add() {
        LoadEsp();
        if (!m_esp) return Failure(m_espError, kUnavailable);
        std::wstring loader;
        auto entry = Core::EnsureLinuxEntry(*m_esp, &loader);
        if (!entry) return Failure(entry.error());
        m_con.Status(Level::Success, std::format(L"{} -> {} ({})", Boot::Name(entry->number), loader,
                                                 entry->created ? L"new entry created" : L"existing entry reused"));
        return kOk;
    }

    // --- entry modifications -------------------------------------------------------
    int Modify(std::wstring_view cmd) {
        auto snap = Snapshot(true);
        if (!snap) return Failure(snap.error());

        if (cmd == L"once" && m_args.Has(kClear)) {
            if (!m_args.positional.empty()) return UsageFailure(m_con, L"'once --clear' takes no entry.");
            if (auto r = Efi::ClearBootNext(); !r) return Failure(r.error());
            m_con.Status(Level::Success, L"BootNext cleared.");
            return kOk;
        }

        // once / default without an entry: the Linux entry, created when missing (as the GUI does).
        std::uint16_t number = 0;
        if ((cmd == L"once" || cmd == L"default") && m_args.positional.empty()) {
            LoadEsp();
            if (!m_esp) return Failure(m_espError, kUnavailable);
            auto entry = Core::EnsureLinuxEntry(*m_esp);
            if (!entry) return Failure(entry.error());
            if (entry->created) m_con.Status(Level::Info, std::format(L"Created {} for Linux.", Boot::Name(entry->number)));
            number = entry->number;
            if (auto fresh = Snapshot(true)) *snap = std::move(*fresh);
        } else {
            auto resolved = ResolveEntry(m_args.positional[0], *snap);
            if (!resolved) return Failure(resolved.error(), kUsage);
            number = *resolved;
        }
        const Boot::Row* row = snap->Find(number);
        const std::wstring name = Describe(number, *snap);
        const int position = row ? row->position : -1;

        Result<> result;
        std::wstring done;
        if (cmd == L"once") {
            result = Efi::SetBootNext(number);
            done = std::format(L"BootNext = {}: the next restart boots it once.", name);
        } else if (cmd == L"default") {
            result = Efi::MoveToFront(number);
            done = std::format(L"{} is now first in BootOrder (default).", name);
        } else if (cmd == L"up" || cmd == L"down") {
            if (position < 0) return Failure(std::format(L"{} is not in BootOrder - use 'default' or 'move' to list it.", name));
            const bool up = cmd == L"up";
            if (up && position == 0) { m_con.Status(Level::Info, std::format(L"{} is already first.", name)); return kOk; }
            if (!up && static_cast<std::size_t>(position) + 1 >= snap->ordered) {
                m_con.Status(Level::Info, std::format(L"{} is already last.", name));
                return kOk;
            }
            const std::size_t target = up ? static_cast<std::size_t>(position) - 1 : static_cast<std::size_t>(position) + 1;
            result = Efi::MoveEntry(number, target);
            done = std::format(L"{} moved to position {}.", name, target + 1);
        } else if (cmd == L"move") {
            const std::wstring& text = m_args.positional[1];
            wchar_t* end = nullptr;
            const unsigned long value = std::wcstoul(text.c_str(), &end, 10);
            if (text.empty() || *end || value == 0) return UsageFailure(m_con, std::format(L"'{}' is not a position (1 = first).", text));
            const std::size_t count = snap->ordered + (position < 0 ? 1 : 0);
            const std::size_t target = std::min<std::size_t>(value, count) - 1;
            result = Efi::MoveEntry(number, target);
            done = std::format(L"{} moved to position {} of {}.", name, target + 1, count);
        } else if (cmd == L"unlist") {
            result = Efi::RemoveFromOrder(number);
            done = std::format(L"{} removed from BootOrder (the entry itself is kept).", name);
        } else if (cmd == L"enable" || cmd == L"disable") {
            const bool enable = cmd == L"enable";
            result = Efi::SetActive(number, enable);
            done = std::format(L"{} {}.", name, enable ? L"enabled" : L"disabled");
        } else if (cmd == L"delete") {
            if (!m_args.Has(kYes)) return Failure(std::format(L"Deleting {} needs --yes.", name), kUsage);
            if (row && row->current && !m_args.Has(kForce))
                return Failure(std::format(L"{} is the entry this system was booted from; add --force to delete it anyway.", name),
                               kUsage);
            if (!row) return Failure(std::format(L"{} does not exist.", name), kUsage);
            auto deleted = Core::DeleteEntry(row->entry);
            if (!deleted) return Failure(deleted.error());
            m_con.Status(Level::Success, std::format(L"{} deleted.", name));
            if (!deleted->bcdCopies.empty())
                m_con.Status(Level::Info, std::format(L"Windows' copy in its BCD store removed as well ({}) - Windows would otherwise "
                                                      L"write the entry back at its next start.", Join(deleted->bcdCopies)));
            if (!deleted->bcdWarning.empty())
                m_con.Status(Level::Warning, std::format(L"The BCD store could not be cleaned: {} - Windows may write the entry back.",
                                                         deleted->bcdWarning));
            return kOk;
        }
        if (!result) return Failure(result.error());
        m_con.Status(Level::Success, done);
        return kOk;
    }

    // True when kernel and initrd are loaded from the boot partition (the ESP folder has no vmlinuz).
    bool KernelOnBootPartition(const Storage::BootPartition& boot) {
        LoadEsp();
        if (!m_esp || !m_esp->layout) return false;
        std::error_code ec;
        return std::filesystem::exists(std::filesystem::path(boot.volume.root) / m_esp->layout->mediaFolder / L"vmlinuz", ec) &&
               !std::filesystem::exists(m_esp->dir / L"vmlinuz", ec);
    }

    // --- boot-partition ---------------------------------------------------------
    int BootPartitionCommand() {
        const bool remove = !m_args.positional.empty();
        if (remove && Lower(m_args.positional[0]) != L"remove")
            return UsageFailure(m_con, std::format(L"Unknown argument '{}' - use 'boot-partition' or 'boot-partition remove --yes'.",
                                                   m_args.positional[0]));
        const auto boot = Storage::FindBootPartition();
        if (!boot) {
            m_con.Status(Level::Info, L"There is no boot partition: kernel and initrd are on the ESP.");
            return kOk;
        }
        const bool inUse = KernelOnBootPartition(*boot);
        if (!remove) {
            m_con.Line(std::format(L"  Boot partition  {}  (FAT32 {}, disk {}, partition {}, {} free, no drive letter)",
                                   boot->volume.root, FormatBytes(boot->size), boot->disk, boot->number, FormatBytes(boot->freeBytes)));
            m_con.Line(inUse ? L"  In use          kernel and initrd are loaded from it (the ESP is too small for them)"
                             : L"  In use          no - the ESP holds kernel and initrd; it can be removed");
            return kOk;
        }
        if (!m_args.Has(kYes)) return Failure(L"Removing the boot partition needs --yes.", kUsage);
        const Install::Layout* layout = m_esp && m_esp->layout ? &*m_esp->layout : nullptr;
        auto data = Storage::LocateData(layout);
        if (!data) return Failure(data.error(), kUnavailable);
        auto removed = Storage::RemoveBootPartition(*boot, data->volume, [this](std::wstring line) { m_con.Status(Level::Info, line); });
        if (!removed) return Failure(removed.error());
        m_con.Status(Level::Success, L"Boot partition removed.");
        if (inUse)
            m_con.Status(Level::Warning, L"Kernel and initrd were on it: Linux Live does not start until 'deploy' puts them back "
                                         L"(on the ESP if it has room by then, otherwise on a new boot partition).");
        return kOk;
    }

    int Reboot() {
        if (!m_args.Has(kYes)) return Failure(L"Restarting needs --yes.", kUsage);
        if (auto r = Core::Restart(m_args.Has(kForce)); !r) return Failure(r.error());
        m_con.Status(Level::Success, L"Restarting.");
        return kOk;
    }

    // --- deploy -----------------------------------------------------------------
    int Deploy() {
        Sources::Pair source = m_args.Has(kGitHub) ? Sources::GitHub() : Sources::Active();
        if (m_args.Has(kImage)) source.image = m_args.image;
        if (m_args.Has(kEsp)) source.esp = m_args.esp;
        LoadEsp();
        if (!m_esp) return Failure(m_espError, kUnavailable);

        m_con.Status(Level::Info, std::format(L"Image   {}", source.image));
        m_con.Status(Level::Info, std::format(L"ESP     {}", source.esp));

        m_con.set_progress_enabled(!m_args.Has(kNoProgress));
        std::stop_source stop;
        g_stop = &stop;
        SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);
        const bool storeKey = m_args.Has(kBitLockerKey);

        Core::Hooks hooks;
        hooks.log = [this](Level level, const std::wstring& text) { m_con.Status(level, text); };
        hooks.progress = [this](const Core::Progress& p) {
            if (p.permille < 0 || p.phase.empty()) return;
            std::wstring detail = p.total ? std::format(L"{} / {}", FormatBytes(p.done), FormatBytes(p.total)) : FormatBytes(p.done);
            if (p.speed) detail += std::format(L"  {}/s", FormatBytes(p.speed));
            if (p.eta > 0) detail += std::format(L"  ETA {}", FormatDuration(static_cast<std::uint64_t>(p.eta)));
            const double fraction = p.total ? static_cast<double>(p.done) / static_cast<double>(p.total) : 0.0;
            m_con.Progress(std::format(L"[{}/{}] {}", p.step, p.steps, p.phase), fraction, detail);
        };
        const bool makePartition = m_args.Has(kBootPartition);
        hooks.confirmBootPartition = [this, makePartition](const std::wstring&) {
            if (!makePartition)
                m_con.Status(Level::Warning, std::format(L"The ESP is too small for kernel and initrd. Add --boot-partition to shrink the Windows "
                                                         L"volume by {} and put them on a FAT32 partition of their own; "
                                                         L"'boot-partition remove --yes' undoes it.",
                                                         FormatBytes(Config::kBootPartitionBytes)));
            return makePartition;
        };
        hooks.confirm = [this, storeKey](const std::wstring&) {
            if (!storeKey)
                m_con.Status(Level::Warning, L"The data volume is BitLocker-encrypted: Linux can only unlock it with the recovery "
                                             L"password on the ESP. Add --bitlocker-key to store it (the ESP is not encrypted).");
            return storeKey;
        };

        auto result = Core::Deploy(source, *m_esp, hooks, stop.get_token());
        m_con.EndProgress();
        SetConsoleCtrlHandler(OnConsoleCtrl, FALSE);
        g_stop = nullptr;
        if (!result) return Failure(result.error(), stop.stop_requested() ? kCanceled : kFailed);
        Storage::UseLayout(*m_esp, *result);

        // The loader is in place: make sure an entry points at it (appended to BootOrder, never the default).
        if (!m_args.Has(kNoEntry)) {
            if (auto access = Core::PrepareFirmwareAccess(); !access) {
                m_con.Status(Level::Warning, access.error());
            } else if (auto entry = Core::EnsureLinuxEntry(*m_esp); !entry) {
                m_con.Status(Level::Warning, entry.error());
            } else {
                m_con.Status(Level::Success, std::format(L"{} {} for Linux.", Boot::Name(entry->number),
                                                         entry->created ? L"created" : L"is ready"));
            }
        }
        m_con.Status(Level::Success, std::format(L"Deployed. Next: '{0} once' and '{0} reboot --yes' to boot Linux Live once.",
                                                 Config::kExeName));
        return kOk;
    }

    Console& m_con;
    const Args& m_args;
    bool m_espLoaded = false;
    std::optional<Storage::EspTarget> m_esp;
    std::wstring m_espError;
};

}  // namespace

std::wstring HelpText() { return HelpTextImpl(); }

int Run(int argc, wchar_t** argv) {
    Text::Use(L"en");  // scripts parse the output: the command line is always English
    Console con;
    const std::span<wchar_t*> args(argv + 1, static_cast<std::size_t>(argc > 1 ? argc - 1 : 0));
    if (std::ranges::any_of(args, [](const wchar_t* a) { return IsHelp(a); })) {
        PrintHelp(con);
        return kOk;
    }
    Args parsed;
    if (auto error = Parse(args, parsed)) return UsageFailure(con, error->message);
    Runner runner(con, parsed);
    return runner.Run();
}

}  // namespace DL::Cli
