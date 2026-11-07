// Main window: volumes, NVRAM snapshot, what is possible right now, status and log.
#include "gui/MainWindow.h"
#include "gui/MainWindowDetail.h"
#include "core/Actions.h"
#include "core/Sources.h"
#include "install/EspInstaller.h"
#include "support/Config.h"
#include "support/Text.h"
#include <algorithm>
#include <filesystem>
#include <format>

namespace DL::Ui {
using namespace MainWindowDetail;

std::wstring MainWindow::SourceSummary() const {
    if (Sources::LoadMode() == Sources::Mode::GitHub)
        return Text::Format(IDS_SUMMARY_GITHUB, Config::kGitHubRepo);
    const auto s = Sources::Active();
    return Text::Format(IDS_SUMMARY_CUSTOM, Sources::Short(s.image), Sources::Short(s.esp));
}

// The ESP first: the package deployed there names both folders (its layout).
void MainWindow::RefreshTargets() {
    std::optional<Install::Layout> layout;
    if (auto esp = Storage::LocateEsp()) {
        m_esp = *esp;
        layout = esp->layout;
        const auto& p = esp->partition;
        auto loader = layout ? Install::FindLoader(esp->dir, layout->espFolder) : Result<std::wstring>(Fail(std::wstring()));
        m_loaderPresent = loader.has_value();
        m_espText = !layout                  ? Text::Get(IDS_ESP_NOT_DEPLOYED)
                    : esp->volume.mount.empty() ? L"\\" + layout->espFolder
                                                : esp->volume.Name() + L"\\" + layout->espFolder;
        m_espDetail = Text::Format(IDS_ESP_DETAIL, p.disk, p.number, p.gpt ? L"GPT" : L"MBR", FormatBytes(p.sizeBytes),
                                   loader ? loader->substr(loader->rfind(L'\\') + 1) : Text::Get(IDS_ESP_NOT_DEPLOYED));
    } else {
        m_esp.reset();
        m_loaderPresent = false;
        m_espText = esp.error();
        m_espDetail.clear();
    }
    // A boot partition exists only for an ESP too small for the kernel; it holds it when the ESP
    // folder has no vmlinuz.
    m_boot = Storage::FindBootPartition();
    m_kernelOnBoot = false;
    if (m_boot && layout && m_esp) {
        std::error_code ec;
        m_kernelOnBoot = std::filesystem::exists(std::filesystem::path(m_boot->volume.root) / layout->mediaFolder / L"vmlinuz", ec) &&
                         !std::filesystem::exists(m_esp->dir / L"vmlinuz", ec);
        if (m_kernelOnBoot) m_espDetail += Text::Format(IDS_BOOTPART_DETAIL, FormatBytes(m_boot->size));
    }
    if (auto data = Storage::LocateData(layout ? &*layout : nullptr)) {
        m_data = *data;
        std::error_code ec;
        const auto size = data->hasSquashfs ? std::filesystem::file_size(data->dir / Config::kSquashfsName, ec) : 0;
        m_dataText = !layout                    ? data->volume.Name()
                     : data->volume.mount.empty() ? data->dir.wstring()
                                                  : data->volume.Name() + L"\\" + layout->mediaFolder;
        m_dataDetail = std::format(L"{}  \x00B7  {}", data->volume.fileSystem,
                                   !layout ? Text::Get(IDS_ESP_NOT_DEPLOYED)
                                   : data->hasSquashfs ? Text::Format(IDS_DATA_IMAGE, FormatBytes(size)) : Text::Get(IDS_DATA_NO_IMAGE));
    } else {
        m_data.reset();
        m_dataText = data.error();
        m_dataDetail.clear();
    }
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void MainWindow::RefreshBoot() {
    if (!m_nvram) {
        m_snapshot = {};
        m_list.SetRows(m_snapshot);
        m_list.SetPlaceholder(Text::Get(m_uefi ? IDS_LIST_NO_NVRAM : IDS_LIST_LEGACY));
        return;
    }
    auto snap = Boot::Read(m_esp ? &m_esp->partition : nullptr,
                           m_esp && m_esp->layout ? std::wstring_view(m_esp->layout->espFolder) : std::wstring_view(), m_showApps);
    if (!snap) {
        Log(Level::Error, snap.error());
        m_list.SetPlaceholder(snap.error());
        return;
    }
    m_snapshot = std::move(*snap);
    m_list.SetRows(m_snapshot);
    m_list.SetPlaceholder(Text::Get(IDS_LIST_EMPTY));
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

bool MainWindow::Can(UINT command) const {
    const Boot::Row* row = m_list.SelectedRow();
    const bool entry = m_nvram && row;
    switch (command) {
    case CmdDeploy: return !m_busy && m_data && m_esp;
    case CmdCancel: return m_busy;
    case CmdOpenData: return m_data && m_data->dirExists;
    case CmdOpenEsp: return m_esp && m_esp->layout && !m_esp->volume.mount.empty();
    case CmdRemoveBootPartition: return !m_busy && m_boot && m_data;
    case CmdAdd: return m_nvram && !m_busy && m_esp && m_loaderPresent;
    case CmdOnce: return entry;
    case CmdDefault: return entry && row->position != 0;
    case CmdUp: return entry && row->position > 0;
    case CmdDown: return entry && row->position >= 0 && static_cast<std::size_t>(row->position) + 1 < m_snapshot.ordered;
    case CmdUnlist: return entry && row->position >= 0 && m_snapshot.ordered > 1;
    case CmdToggleActive: case CmdDelete: case CmdCopyEntry: return entry;
    case CmdClearNext: return m_nvram && m_snapshot.next.has_value();
    case CmdCopyLog: case CmdClearLog: return m_log.Count() > 0;
    default: return true;
    }
}

void MainWindow::UpdateControls() {
    for (UINT id : {CmdDeploy, CmdCancel, CmdUp, CmdDown, CmdOnce, CmdDefault, CmdAdd, CmdDelete}) {
        HWND b = GetDlgItem(m_hwnd, static_cast<int>(id));
        const bool enable = Can(id);
        if ((IsWindowEnabled(b) != FALSE) != enable) {
            // Never disable the control that has the focus without moving the focus first.
            if (!enable && GetFocus() == b) SendMessageW(m_hwnd, WM_NEXTDLGCTL, 0, FALSE);
            EnableWindow(b, enable);
        }
    }
}

void MainWindow::SetStatus(Level level, std::wstring text) {
    m_statusLevel = level;
    m_status = std::move(text);
    const RECT r{static_cast<LONG>(m_statusRect.left), static_cast<LONG>(m_statusRect.top), static_cast<LONG>(m_statusRect.right),
                 static_cast<LONG>(m_statusRect.bottom)};
    InvalidateRect(m_hwnd, &r, FALSE);
}

void MainWindow::Log(Level level, std::wstring text) {
    m_log.Append(level, std::move(text));
    if (!m_logVisible) {
        ++m_unread;
        m_looks[CmdExpand].badge = m_unread > 99 ? std::wstring(L"99+") : std::to_wstring(m_unread);
        InvalidateRect(GetDlgItem(m_hwnd, CmdExpand), nullptr, FALSE);
    }
}

void MainWindow::SetTaskbar(TBPFLAG state, int permille) {
    if (!m_taskbar) return;
    m_taskbar->SetProgressState(m_hwnd, state);
    if (state == TBPF_NORMAL || state == TBPF_ERROR) m_taskbar->SetProgressValue(m_hwnd, static_cast<ULONGLONG>(std::max(permille, 0)), 1000);
}

void MainWindow::StartAnimation() { SetTimer(m_hwnd, kAnimationTimer, 15, nullptr); }

}  // namespace DL::Ui
