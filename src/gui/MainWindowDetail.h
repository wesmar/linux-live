// Internal to the MainWindow*.cpp files: command identifiers, worker messages, layout constants.
#pragma once
#include "core/Deploy.h"
#include "efi/Efi.h"
#include "gui/Widgets.h"
#include "support/Text.h"
#include <Windows.h>
#include <cwctype>
#include <format>
#include <memory>
#include <string>

namespace DL::Ui::MainWindowDetail {

inline constexpr wchar_t kClassName[] = L"LinuxLive.MainWindow";
inline constexpr UINT_PTR kAnimationTimer = 1;

enum : UINT {
    WM_APP_LOG = WM_APP + 1,  // lParam: LogMessage*
    WM_APP_PROGRESS,          // lParam: Core::Progress*
    WM_APP_DONE,              // lParam: DoneMessage*
    WM_APP_CONFIRM,           // wParam: ConfirmKind, lParam: const std::wstring* (sent, result 1 = yes)
    WM_APP_REFRESH,
};

enum Command : UINT {
    CmdDeploy = 1000, CmdCancel, CmdRefresh, CmdOpenData, CmdOpenEsp, CmdExit,
    CmdSourceGitHub, CmdSourceCustom, CmdSourceEdit, CmdReleases, CmdSourceMenu,
    CmdAdd, CmdOnce, CmdClearNext, CmdDefault, CmdUp, CmdDown, CmdUnlist, CmdToggleActive, CmdDelete,
    CmdCopyEntry, CmdRestart,
    CmdViewLog, CmdViewApps, CmdCopyLog, CmdClearLog, CmdExpand,
    CmdHelpCli, CmdAbout, CmdRemoveBootPartition,
    CmdLanguageAuto = 1100, CmdLanguageFirst,  // View > language: automatic, then one per available language
    CmdLanguageLast = 1199,
};
inline constexpr int kIdList = 2000, kIdLog = 2001;

// The questions a deployment can ask (WM_APP_CONFIRM).
enum ConfirmKind : WPARAM { ConfirmBitLocker = 0, ConfirmBootPartition = 1 };

struct LogMessage {
    Level level;
    std::wstring text;
};
struct DoneMessage {
    bool ok, canceled;
    std::wstring error;
};

// The worker hands objects to the UI thread through the message queue. If posting fails
// (window already gone) the unique_ptr still owns the object and frees it.
template <class T>
void PostOwned(HWND hwnd, UINT message, std::unique_ptr<T> payload) {
    if (PostMessageW(hwnd, message, 0, reinterpret_cast<LPARAM>(payload.get()))) (void)payload.release();
}
template <class T>
std::unique_ptr<T> TakeOwned(LPARAM lParam) {
    return std::unique_ptr<T>(reinterpret_cast<T*>(lParam));
}

inline std::wstring ProgressText(const Core::Progress& p) {
    if (p.permille < 0) return p.phase + L"\x2026";
    std::wstring text = std::format(L"{}%  \x00B7  ", p.permille / 10);
    if (p.steps > 1) text += std::format(L"{}/{}  ", p.step, p.steps);
    text += p.phase;
    if (p.total) text += std::format(L"  \x00B7  {} / {}", FormatBytes(p.done), FormatBytes(p.total));
    if (p.speed) text += std::format(L"  \x00B7  {}/s", FormatBytes(p.speed));
    if (p.eta > 0) text += L"  \x00B7  " + Text::Format(IDS_PROGRESS_LEFT, FormatDuration(static_cast<std::uint64_t>(p.eta)));
    return text;
}

inline bool IsWindowsBootManager(const Efi::BootEntry& e) {
    std::wstring path = e.filePath;
    for (auto& c : path) c = static_cast<wchar_t>(std::towlower(c));
    return path == L"\\efi\\microsoft\\boot\\bootmgfw.efi";
}


// Every button of the window: command, string, look. The source "combo" gets its text at runtime.
struct ButtonSpec {
    UINT id;
    UINT text;  // string id, 0 = set later
    ButtonLook look;
};
inline const ButtonSpec kButtons[] = {
    {CmdSourceMenu, 0, {ButtonKind::Combo}},
    {CmdCancel, IDS_CANCEL, {ButtonKind::Standard, Icon::Cancel}},
    {CmdDeploy, IDS_BTN_DEPLOY, {ButtonKind::Accent, Icon::Download}},
    {CmdUp, IDS_BTN_UP, {ButtonKind::Standard, Icon::ArrowUp}},
    {CmdDown, IDS_BTN_DOWN, {ButtonKind::Standard, Icon::ArrowDown}},
    {CmdOnce, IDS_BTN_ONCE, {ButtonKind::Standard, Icon::Play}},
    {CmdDefault, IDS_BTN_DEFAULT, {ButtonKind::Standard, Icon::Pin}},
    {CmdAdd, IDS_BTN_ADD, {ButtonKind::Standard, Icon::Add}},
    {CmdDelete, IDS_BTN_DELETE, {ButtonKind::Standard, Icon::Delete}},
    {CmdExpand, IDS_BTN_LOG, {ButtonKind::Expander, 0, false}},
};

// Layout, in DIP (scaled with the window DPI).
inline constexpr float kMargin = 12, kPad = 12, kButtonW = 150, kButtonH = 30, kGap = 6, kGroupGap = 12;
inline constexpr float kBarH = 28, kLogH = 190;

}  // namespace DL::Ui::MainWindowDetail
