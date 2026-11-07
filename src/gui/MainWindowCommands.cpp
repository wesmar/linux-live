// Main window: commands - deployment, boot entries, sources, language, restart and the help dialogs.
#include "gui/MainWindow.h"
#include "gui/MainWindowDetail.h"
#include "app/Cli.h"
#include "core/Actions.h"
#include "core/Sources.h"
#include "gui/Dialogs.h"
#include "support/Config.h"
#include "support/Settings.h"
#include "support/Text.h"
#include <shellapi.h>
#include <algorithm>
#include <format>

#pragma comment(lib, "shell32.lib")

namespace DL::Ui {
using namespace MainWindowDetail;

void MainWindow::OnCommand(UINT id, UINT code, HWND control) {
    if (control && id == kIdList) {
        if (code == LBN_SELCHANGE) UpdateControls();
        return;
    }
    if (id == IDCANCEL) id = CmdCancel;  // Esc
    if (id == IDOK) return;               // Enter: no default action
    if (id >= CmdLanguageAuto && id <= CmdLanguageLast) {
        SetLanguage(id);
        return;
    }
    if (!Can(id)) return;
    switch (id) {
    case CmdDeploy: StartDeploy(); break;
    case CmdCancel:
        m_worker.request_stop();
        SetStatus(Level::Warning, Text::Get(IDS_STATUS_CANCELING));
        break;
    case CmdRefresh:
        RefreshTargets();
        RefreshBoot();
        UpdateControls();
        SetStatus(Level::Info, Text::Get(IDS_STATUS_REFRESHED));
        break;
    case CmdOpenData: OpenFolder(false); break;
    case CmdOpenEsp: OpenFolder(true); break;
    case CmdExit: PostMessageW(m_hwnd, WM_CLOSE, 0, 0); break;
    case CmdSourceMenu: {
        RECT r{};
        GetWindowRect(GetDlgItem(m_hwnd, CmdSourceMenu), &r);
        m_menu.TrackContext(m_hwnd, m_sourceMenu, {r.left, r.bottom + m_fonts.S(2)});
        break;
    }
    case CmdSourceGitHub: SetSourceMode(false); break;
    case CmdSourceCustom: SetSourceMode(true); break;
    case CmdSourceEdit: EditCustomSources(); break;
    case CmdReleases: ShellExecuteW(m_hwnd, L"open", Sources::GitHubReleasesPage().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case CmdRestart: RestartNow(); break;
    case CmdViewLog:
    case CmdExpand: SetLogVisible(!m_logVisible, true); break;
    case CmdViewApps:
        m_showApps = !m_showApps;
        Settings::WriteDword(L"ShowFirmwareApps", m_showApps ? 1 : 0);
        RefreshBoot();
        UpdateControls();
        break;
    case CmdCopyLog: CopyToClipboard(m_hwnd, m_log.Text()); break;
    case CmdClearLog: m_log.Clear(); UpdateControls(); break;
    case CmdHelpCli: ShowCommandLineHelp(); break;
    case CmdAbout: ShowAbout(); break;
    case CmdRemoveBootPartition: RemoveBootPartition(); break;
    default: BootAction(id); break;
    }
}

// ---------------------------------------------------------------------------
// Language
// ---------------------------------------------------------------------------
void MainWindow::SetLanguage(UINT command) {
    const std::wstring code = command == CmdLanguageAuto ? std::wstring(L"auto")
                              : command - CmdLanguageFirst < m_languages.size() ? m_languages[command - CmdLanguageFirst].code
                                                                                 : std::wstring();
    if (code.empty() || !Text::Use(code, true)) return;
    ApplyLanguage();
    Log(Level::Info, Text::Format(IDS_LANGUAGE_CHANGED, LanguageName(Text::Current())));
}

std::wstring MainWindow::LanguageName(const std::wstring& code) const {
    for (const auto& l : m_languages)
        if (l.code == code) return l.name;
    return code;
}

// Everything that shows text is rebuilt; layout follows (labels change width, buttons stay equal).
void MainWindow::ApplyLanguage() {
    m_menu.Reset();
    BuildMenus();
    for (const ButtonSpec& b : kButtons)
        if (b.text) SetWindowTextW(GetDlgItem(m_hwnd, static_cast<int>(b.id)), Text::Get(b.text).c_str());
    SetWindowTextW(GetDlgItem(m_hwnd, CmdSourceMenu), SourceSummary().c_str());
    RefreshTargets();
    RefreshBoot();
    SetStatus(Level::Info, Text::Get(IDS_STATUS_READY));
    Layout();
    RedrawWindow(m_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

// ---------------------------------------------------------------------------
// Sources
// ---------------------------------------------------------------------------
void MainWindow::SetSourceMode(bool custom) {
    if (custom) {
        const auto saved = Sources::LoadCustom();
        if (saved.image.empty() || saved.esp.empty()) {
            EditCustomSources();  // nothing to switch to yet
            return;
        }
    }
    Sources::SaveMode(custom ? Sources::Mode::Custom : Sources::Mode::GitHub);
    SetWindowTextW(GetDlgItem(m_hwnd, CmdSourceMenu), SourceSummary().c_str());
    InvalidateRect(GetDlgItem(m_hwnd, CmdSourceMenu), nullptr, FALSE);
    Log(Level::Info, Text::Format(IDS_LOG_SOURCE, SourceSummary()));
}

void MainWindow::EditCustomSources() {
    // File dialog filters: "name\0pattern\0" pairs, built from translated names.
    auto filter = [](UINT name, const wchar_t* pattern) {
        std::wstring f = Text::Get(name);
        f += L'\0';
        f += pattern;
        f += L'\0';
        f += Text::Get(IDS_FILTER_ALL);
        f += L'\0';
        f += L"*.*";
        f += L'\0';
        return f;
    };
    const std::wstring imageFilter = filter(IDS_FILTER_SQUASHFS, L"*.squashfs");
    const std::wstring packageFilter = filter(IDS_FILTER_TAR, L"*.tar");

    const auto current = Sources::LoadCustom();
    DialogSpec spec;
    spec.heading = Text::Get(IDS_SOURCES_HEADING);
    spec.message = Text::Get(IDS_SOURCES_MESSAGE);
    spec.fields = {
        {Text::Get(IDS_SOURCES_IMAGE), current.image, Text::Get(IDS_SOURCES_IMAGE_CUE), imageFilter.c_str()},
        {Text::Get(IDS_SOURCES_PACKAGE), current.esp, Text::Get(IDS_SOURCES_PACKAGE_CUE), packageFilter.c_str()},
    };
    spec.ok = Text::Get(IDS_SOURCES_OK);
    spec.extra = Text::Get(IDS_SOURCES_GITHUB);
    spec.width = 520;
    spec.validate = [](const std::vector<std::wstring>& v) -> std::optional<std::wstring> {
        if (auto r = Sources::Check(v[0], Sources::Part::Image); !r) return r.error();
        if (auto r = Sources::Check(v[1], Sources::Part::Package); !r) return r.error();
        return std::nullopt;
    };
    m_modal = true;
    const auto result = RunDialog(m_hwnd, spec);
    m_modal = false;
    if (result.button == 2) {
        SetSourceMode(false);
    } else if (result.button == 1) {
        Sources::SaveCustom({result.values[0], result.values[1]});
        SetSourceMode(true);
    }
}

// ---------------------------------------------------------------------------
// Deployment
// ---------------------------------------------------------------------------
void MainWindow::StartDeploy() {
    RefreshTargets();
    if (!m_data || !m_esp) {
        SetStatus(Level::Error, m_data ? m_espText : m_dataText);
        return;
    }
    const Sources::Pair source = Sources::Active();
    m_busy = true;
    m_failed = m_finished = false;
    m_progress = {};
    m_shown = 0;
    m_animStart = GetTickCount64();
    UpdateControls();
    SetTaskbar(TBPF_INDETERMINATE);
    StartAnimation();
    Log(Level::Info, Text::Format(IDS_LOG_DEPLOYING_FROM, SourceSummary()));

    HWND hwnd = m_hwnd;
    const Storage::EspTarget esp = *m_esp;
    m_worker = std::jthread([hwnd, source, esp](std::stop_token stop) {
        Core::Hooks hooks;
        hooks.log = [hwnd](Level level, const std::wstring& text) {
            PostOwned(hwnd, WM_APP_LOG, std::make_unique<LogMessage>(LogMessage{level, text}));
        };
        hooks.progress = [hwnd](const Core::Progress& p) { PostOwned(hwnd, WM_APP_PROGRESS, std::make_unique<Core::Progress>(p)); };
        hooks.confirm = [hwnd](const std::wstring& question) {
            // Synchronous: the worker waits for the answer. Fails (no) when the window is gone.
            return SendMessageW(hwnd, WM_APP_CONFIRM, ConfirmBitLocker, reinterpret_cast<LPARAM>(&question)) == 1;
        };
        hooks.confirmBootPartition = [hwnd](const std::wstring& question) {
            return SendMessageW(hwnd, WM_APP_CONFIRM, ConfirmBootPartition, reinterpret_cast<LPARAM>(&question)) == 1;
        };
        auto result = Core::Deploy(source, esp, hooks, stop);
        PostOwned(hwnd, WM_APP_DONE,
                  std::make_unique<DoneMessage>(DoneMessage{result.has_value(), stop.stop_requested(), result ? std::wstring() : result.error()}));
    });
}

void MainWindow::OnDeployDone(bool ok, bool canceled, const std::wstring& error) {
    m_busy = false;
    m_progress.permille = ok ? 1000 : std::max(m_progress.permille, 0);
    m_finished = ok;
    m_failed = !ok;
    StartAnimation();
    RefreshTargets();
    if (ok) {
        SetTaskbar(TBPF_NOPROGRESS);
        // The loader is in place now: make sure an entry points at it (appended, never made default).
        if (m_nvram && m_esp) {
            if (auto entry = Core::EnsureLinuxEntry(*m_esp)) {
                Log(Level::Success, Text::Format(entry->created ? IDS_LOG_ENTRY_CREATED : IDS_LOG_ENTRY_READY, Boot::Name(entry->number)));
                RefreshBoot();
                m_list.Select(entry->number);
            } else {
                Log(Level::Warning, entry.error());
            }
        }
        Log(Level::Success, Text::Get(IDS_LOG_DEPLOY_DONE));
        SetStatus(Level::Success, Text::Get(IDS_STATUS_DEPLOYED));
    } else {
        SetTaskbar(canceled ? TBPF_NOPROGRESS : TBPF_ERROR, m_progress.permille);
        Log(canceled ? Level::Warning : Level::Error, error);
        SetStatus(canceled ? Level::Warning : Level::Error, canceled ? Text::Get(IDS_CANCELED) : error);
        if (!canceled) FlashWindow(m_hwnd, TRUE);
    }
    UpdateControls();
}

// ---------------------------------------------------------------------------
// Boot entries
// ---------------------------------------------------------------------------
void MainWindow::BootAction(UINT command) {
    const Boot::Row* selected = m_list.SelectedRow();
    if (command != CmdAdd && command != CmdClearNext && !selected) return;
    const Boot::Row row = selected ? *selected : Boot::Row{};
    const std::uint16_t number = row.entry.number;
    const std::wstring name = Text::Format(IDS_ENTRY_NAME, Boot::Name(number), row.entry.description);
    std::optional<std::uint16_t> select = number;
    Result<> result;
    std::wstring done;

    switch (command) {
    case CmdAdd: {
        std::wstring loader;
        auto entry = Core::EnsureLinuxEntry(*m_esp, &loader);
        if (!entry) { result = Fail(entry.error()); break; }
        select = entry->number;
        done = Text::Format(IDS_DONE_ADD, Boot::Name(entry->number), loader, Text::Get(entry->created ? IDS_ADD_CREATED : IDS_ADD_REUSED));
        break;
    }
    case CmdOnce:
        result = Efi::SetBootNext(number);
        done = Text::Format(IDS_DONE_ONCE, name);
        break;
    case CmdClearNext:
        result = Efi::ClearBootNext();
        done = Text::Get(IDS_DONE_CLEAR_NEXT);
        break;
    case CmdDefault:
        result = Efi::MoveToFront(number);
        done = Text::Format(IDS_DONE_DEFAULT, name);
        break;
    case CmdUp:
    case CmdDown: {
        const std::size_t to = static_cast<std::size_t>(row.position) + (command == CmdUp ? std::size_t(-1) : 1);
        result = Efi::MoveEntry(number, to);
        done = Text::Format(IDS_DONE_MOVED, name, to + 1);
        break;
    }
    case CmdUnlist:
        result = Efi::RemoveFromOrder(number);
        done = Text::Format(IDS_DONE_UNLIST, name);
        break;
    case CmdToggleActive: {
        const bool enable = !row.entry.Active();
        if (!enable && row.current) {
            m_modal = true;
            const bool yes = Confirm(m_hwnd, Text::Get(IDS_DISABLE_HEADING), Text::Format(IDS_DISABLE_MESSAGE, name),
                                     Text::Get(IDS_DISABLE_OK), true);
            m_modal = false;
            if (!yes) return;
        }
        result = Efi::SetActive(number, enable);
        done = Text::Format(enable ? IDS_DONE_ENABLED : IDS_DONE_DISABLED, name);
        break;
    }
    case CmdDelete: {
        const bool critical = row.current || IsWindowsBootManager(row.entry);
        std::wstring message = Text::Format(IDS_DELETE_MESSAGE, name, Boot::Target(row.entry));
        if (critical) message += Text::Get(IDS_DELETE_CRITICAL);
        m_modal = true;
        const bool yes = Confirm(m_hwnd, Text::Format(IDS_DELETE_HEADING, Boot::Name(number)), message, Text::Get(IDS_DELETE_OK), critical);
        m_modal = false;
        if (!yes) return;
        if (auto deleted = Core::DeleteEntry(row.entry); !deleted) {
            result = Fail(deleted.error());
        } else {
            // Windows keeps copies of some entries in its BCD store and writes them back otherwise.
            std::wstring ids;
            for (const auto& id : deleted->bcdCopies) ids += (ids.empty() ? L"" : L", ") + id;
            if (!ids.empty()) Log(Level::Info, Text::Format(IDS_BCD_COPY_REMOVED, ids));
            if (!deleted->bcdWarning.empty()) Log(Level::Warning, Text::Format(IDS_BCD_COPY_LEFT, deleted->bcdWarning));
        }
        done = Text::Format(IDS_DONE_DELETED, name);
        select.reset();
        break;
    }
    case CmdCopyEntry: {
        // Technical record for a bug report or a forum post: always English, tab separated.
        const std::wstring text = std::format(L"{}\t{}\t{}\tattributes 0x{:08X}{}", Boot::Name(number), row.entry.description,
                                              Boot::Target(row.entry), row.entry.attributes,
                                              row.position >= 0 ? std::format(L"\tposition {}", row.position + 1) : std::wstring());
        CopyToClipboard(m_hwnd, text);
        SetStatus(Level::Info, Text::Get(IDS_STATUS_COPIED));
        return;
    }
    default:
        return;
    }

    if (!result) {
        Log(Level::Error, result.error());
        SetStatus(Level::Error, result.error());
    } else {
        Log(Level::Success, done);
        SetStatus(Level::Success, done);
    }
    RefreshBoot();
    if (select) m_list.Select(*select);
    UpdateControls();

    if (result && command == CmdOnce) {
        m_modal = true;
        const bool restart = Confirm(m_hwnd, Text::Get(IDS_RESTART_HEADING), Text::Format(IDS_RESTART_AFTER_ONCE, name),
                                     Text::Get(IDS_RESTART_OK));
        m_modal = false;
        if (restart) {
            if (auto r = Core::Restart(false); !r) {
                Log(Level::Error, r.error());
                SetStatus(Level::Error, r.error());
            }
        }
    }
}

void MainWindow::RestartNow() {
    m_modal = true;
    const bool yes = Confirm(m_hwnd, Text::Get(IDS_RESTART_HEADING),
                             m_snapshot.next ? Text::Format(IDS_RESTART_NEXT, Boot::Name(*m_snapshot.next)) : Text::Get(IDS_RESTART_DEFAULT),
                             Text::Get(IDS_RESTART_OK));
    m_modal = false;
    if (!yes) return;
    if (auto r = Core::Restart(false); !r) {
        Log(Level::Error, r.error());
        SetStatus(Level::Error, r.error());
    }
}

// Deletes the boot partition and gives its space back to the Windows volume (a few seconds).
void MainWindow::RemoveBootPartition() {
    if (!m_boot || !m_data) return;
    std::wstring message = Text::Format(IDS_BOOTPART_REMOVE_MESSAGE, FormatBytes(m_boot->size), m_data->volume.Name());
    if (m_kernelOnBoot) message += Text::Get(IDS_BOOTPART_REMOVE_IN_USE);
    m_modal = true;
    const bool yes = Confirm(m_hwnd, Text::Get(IDS_BOOTPART_REMOVE_HEADING), message, Text::Get(IDS_BOOTPART_REMOVE_OK), m_kernelOnBoot);
    m_modal = false;
    if (!yes) return;
    const HCURSOR previous = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    auto r = Storage::RemoveBootPartition(*m_boot, m_data->volume, [this](std::wstring line) { Log(Level::Info, line); });
    SetCursor(previous);
    if (!r) {
        Log(Level::Error, r.error());
        SetStatus(Level::Error, r.error());
    } else {
        SetStatus(Level::Success, Text::Get(IDS_BOOTPART_REMOVED_STATUS));
    }
    RefreshTargets();
    UpdateControls();
}

void MainWindow::OpenFolder(bool esp) {
    std::wstring path;
    if (esp && m_esp && m_esp->layout) path = m_esp->volume.Name() + L"\\" + m_esp->layout->espFolder;
    else if (!esp && m_data && !m_data->dir.empty()) path = m_data->dir.wstring();
    if (!path.empty()) ShellExecuteW(m_hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------------------
// Help
// ---------------------------------------------------------------------------
void MainWindow::ShowCommandLineHelp() {
    DialogSpec spec;
    spec.heading = Text::Get(IDS_HELP_HEADING);
    spec.message = Text::Format(IDS_HELP_MESSAGE, Config::kExeName);
    spec.monospace = Cli::HelpText();  // the command line is English, so is its help
    spec.ok = Text::Get(IDS_CLOSE);
    spec.cancelButton = false;
    spec.extra = Text::Get(IDS_COPY);
    spec.width = 720;
    m_modal = true;
    const auto result = RunDialog(m_hwnd, spec);
    m_modal = false;
    if (result.button == 2) CopyToClipboard(m_hwnd, spec.monospace);
}

void MainWindow::ShowAbout() {
    m_modal = true;
    Inform(m_hwnd, std::format(L"{} {}", Config::kAppTitle, Config::kAppVersion),
           Text::Format(IDS_ABOUT_MESSAGE, Config::kGitHubRepo, Config::kCopyright, Config::kGitHubRepo));
    m_modal = false;
}

}  // namespace DL::Ui
