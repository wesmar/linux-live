// Main window: menu bar, context menus, accelerators and the enabled / checked state of items.
#include "gui/MainWindow.h"
#include "gui/MainWindowDetail.h"
#include "core/Sources.h"
#include "support/Text.h"
#include <format>

namespace DL::Ui {
using namespace MainWindowDetail;

void MainWindow::BuildMenus() {
    auto& m = m_menu;
    auto item = [&m](HMENU menu, UINT id, UINT text, const wchar_t* shortcut = L"", bool radio = false) {
        m.AddItem(menu, id, Text::Get(text), shortcut, radio);
    };

    HMENU file = m.AddMenu(Text::Get(IDS_MENU_FILE));
    item(file, CmdDeploy, IDS_MI_DEPLOY, L"Ctrl+D");
    item(file, CmdCancel, IDS_CANCEL, L"Esc");
    m.AddSeparator(file);
    item(file, CmdRefresh, IDS_MI_REFRESH, L"F5");
    item(file, CmdOpenData, IDS_MI_OPEN_DATA);
    item(file, CmdOpenEsp, IDS_MI_OPEN_ESP);
    item(file, CmdRemoveBootPartition, IDS_MI_REMOVE_BOOTPART);
    m.AddSeparator(file);
    item(file, CmdExit, IDS_MI_EXIT, L"Ctrl+Q");

    m_sourceMenu = m.AddMenu(Text::Get(IDS_MENU_SOURCE));
    item(m_sourceMenu, CmdSourceGitHub, IDS_MI_SRC_GITHUB, L"", true);
    item(m_sourceMenu, CmdSourceCustom, IDS_MI_SRC_CUSTOM, L"", true);
    m.AddSeparator(m_sourceMenu);
    item(m_sourceMenu, CmdSourceEdit, IDS_MI_SRC_EDIT, L"Ctrl+E");
    item(m_sourceMenu, CmdReleases, IDS_MI_RELEASES);

    HMENU boot = m.AddMenu(Text::Get(IDS_MENU_BOOT));
    item(boot, CmdAdd, IDS_MI_ADD, L"Ctrl+N");
    m.AddSeparator(boot);
    item(boot, CmdOnce, IDS_MI_ONCE, L"Ctrl+B");
    item(boot, CmdDefault, IDS_MI_DEFAULT, L"Ctrl+Home");
    item(boot, CmdUp, IDS_MI_UP, L"Ctrl+Up");
    item(boot, CmdDown, IDS_MI_DOWN, L"Ctrl+Down");
    m.AddSeparator(boot);
    item(boot, CmdToggleActive, IDS_MI_ENABLED);
    item(boot, CmdUnlist, IDS_MI_UNLIST);
    item(boot, CmdDelete, IDS_MI_DELETE, L"Del");
    m.AddSeparator(boot);
    item(boot, CmdClearNext, IDS_MI_CLEAR_NEXT);
    item(boot, CmdCopyEntry, IDS_MI_COPY_ENTRY);
    m.AddSeparator(boot);
    item(boot, CmdRestart, IDS_MI_RESTART);

    HMENU view = m.AddMenu(Text::Get(IDS_MENU_VIEW));
    item(view, CmdViewLog, IDS_MI_LOG_PANEL, L"Ctrl+L");
    item(view, CmdViewApps, IDS_MI_APPS);
    m.AddSeparator(view);
    item(view, CmdCopyLog, IDS_MI_COPY_LOG);
    item(view, CmdClearLog, IDS_MI_CLEAR_LOG);
    // Language: follow Windows, or English (built in) / one of the language files, each named in itself
    // and with its coverage, so a half-done translation is visible. Long lists continue in columns.
    m.AddSeparator(view);
    HMENU language = m.AddSubmenu(view, Text::Get(IDS_MI_LANGUAGE));
    m_languages = Text::Available();
    m.AddItem(language, CmdLanguageAuto, Text::Format(IDS_LANGUAGE_AUTO, LanguageName(Text::Detect())), {}, true);
    m.AddSeparator(language);
    constexpr std::size_t kPerColumn = 20;
    for (std::size_t i = 0; i < m_languages.size() && CmdLanguageFirst + i <= CmdLanguageLast; ++i) {
        const Text::Language& l = m_languages[i];
        m.AddItem(language, static_cast<UINT>(CmdLanguageFirst + i), l.name, l.code == L"en" ? std::wstring() : std::format(L"{}%", l.Coverage()),
                  true, i > 0 && i % kPerColumn == 0);
    }

    HMENU help = m.AddMenu(Text::Get(IDS_MENU_HELP));
    item(help, CmdHelpCli, IDS_MI_HELP_CLI, L"F1");
    item(help, CmdAbout, IDS_MI_ABOUT);

    m_listMenu = m.CreatePopup();
    item(m_listMenu, CmdOnce, IDS_MI_ONCE, L"Ctrl+B");
    item(m_listMenu, CmdDefault, IDS_MI_DEFAULT, L"Ctrl+Home");
    item(m_listMenu, CmdUp, IDS_MI_UP, L"Ctrl+Up");
    item(m_listMenu, CmdDown, IDS_MI_DOWN, L"Ctrl+Down");
    m.AddSeparator(m_listMenu);
    item(m_listMenu, CmdToggleActive, IDS_MI_ENABLED);
    item(m_listMenu, CmdUnlist, IDS_MI_UNLIST);
    item(m_listMenu, CmdDelete, IDS_MI_DELETE, L"Del");
    m.AddSeparator(m_listMenu);
    item(m_listMenu, CmdCopyEntry, IDS_MI_COPY_ENTRY);

    m_logMenu = m.CreatePopup();
    item(m_logMenu, CmdCopyLog, IDS_MI_COPY_LOG, L"Ctrl+C");
    item(m_logMenu, CmdClearLog, IDS_MI_CLEAR_LOG);

    RECT client{};
    GetClientRect(m_hwnd, &client);
    if (client.right > 0) Layout();  // the menu bar width depends on the language
}

void MainWindow::BuildAccelerators() {
    ACCEL keys[] = {
        {FVIRTKEY | FCONTROL, 'D', CmdDeploy},     {FVIRTKEY, VK_F5, CmdRefresh},
        {FVIRTKEY | FCONTROL, 'Q', CmdExit},       {FVIRTKEY | FCONTROL, 'E', CmdSourceEdit},
        {FVIRTKEY | FCONTROL, 'N', CmdAdd},        {FVIRTKEY | FCONTROL, 'B', CmdOnce},
        {FVIRTKEY | FCONTROL, VK_HOME, CmdDefault}, {FVIRTKEY | FCONTROL, VK_UP, CmdUp},
        {FVIRTKEY | FCONTROL, VK_DOWN, CmdDown},   {FVIRTKEY, VK_DELETE, CmdDelete},
        {FVIRTKEY | FCONTROL, 'L', CmdViewLog},    {FVIRTKEY, VK_F1, CmdHelpCli},
    };
    m_accel.reset(CreateAcceleratorTableW(keys, static_cast<int>(std::size(keys))));
}

// Enabled / checked state of every item of `menu`, from the same rules as the buttons.
void MainWindow::ApplyMenuState(HMENU menu) const {
    const bool custom = Sources::LoadMode() == Sources::Mode::Custom;
    const Boot::Row* row = m_list.SelectedRow();
    const bool automatic = Text::Automatic();
    const std::wstring language = Text::Current();
    for (int i = 0; i < GetMenuItemCount(menu); ++i) {
        const UINT id = GetMenuItemID(menu, i);
        if (id == static_cast<UINT>(-1) || id == 0) continue;  // separator
        EnableMenuItem(menu, id, MF_BYCOMMAND | (Can(id) ? MF_ENABLED : MF_GRAYED));
        bool checked = false;
        if (id == CmdLanguageAuto) {
            checked = automatic;
        } else if (id >= CmdLanguageFirst && id <= CmdLanguageLast) {
            const std::size_t index = id - CmdLanguageFirst;
            checked = !automatic && index < m_languages.size() && m_languages[index].code == language;
        } else {
            switch (id) {
            case CmdSourceGitHub: checked = !custom; break;
            case CmdSourceCustom: checked = custom; break;
            case CmdViewLog: checked = m_logVisible; break;
            case CmdViewApps: checked = m_showApps; break;
            case CmdToggleActive: checked = row && row->entry.Active(); break;
            default: continue;
            }
        }
        CheckMenuItem(menu, id, MF_BYCOMMAND | (checked ? MF_CHECKED : MF_UNCHECKED));
    }
}

void MainWindow::ShowContextMenu(HWND target, POINT screen) {
    if (target == m_list.hwnd()) m_menu.TrackContext(m_hwnd, m_listMenu, screen);
    else if (target == m_log.hwnd()) m_menu.TrackContext(m_hwnd, m_logMenu, screen);
}

}  // namespace DL::Ui
