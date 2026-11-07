#include "gui/Menus.h"
#include "gui/Widgets.h"
#include <algorithm>
#include <cwctype>

namespace DL::Ui {
namespace {
MenuBar* g_tracking = nullptr;  // the bar whose menu loop is running (one per thread at most)
}

MenuBar::MenuBar(const Fonts& fonts) : m_fonts(fonts), m_background(CreateSolidBrush(ToColorRef(Color::Flyout))) {}

MenuBar::~MenuBar() = default;

void MenuBar::Prepare(HMENU menu) {
    MENUINFO info{sizeof info};
    info.fMask = MIM_BACKGROUND | MIM_STYLE;
    info.hbrBack = m_background.get();
    info.dwStyle = MNS_CHECKORBMP;
    SetMenuInfo(menu, &info);
}

HMENU MenuBar::AddMenu(std::wstring label) {
    wchar_t mnemonic = 0;
    if (const std::size_t amp = label.find(L'&'); amp != std::wstring::npos && amp + 1 < label.size()) {
        mnemonic = static_cast<wchar_t>(std::towlower(label[amp + 1]));
        label.erase(amp, 1);
    }
    MenuHandle menu(CreatePopupMenu());
    HMENU handle = menu.get();
    Prepare(handle);
    m_tops.push_back({std::move(label), mnemonic, std::move(menu), {}});
    return handle;
}

void MenuBar::Reset() {
    m_tops.clear();    // menus first: their items point into m_data
    m_popups.clear();
    m_data.clear();
    m_hot = m_open = -1;
}

HMENU MenuBar::CreatePopup() {
    MenuHandle menu(CreatePopupMenu());
    HMENU handle = menu.get();
    Prepare(handle);
    m_popups.push_back(std::move(menu));
    return handle;
}

void MenuBar::AddItem(HMENU menu, UINT id, std::wstring text, std::wstring shortcut, bool radio, bool column) {
    auto data = std::make_unique<Data>(Data{std::move(text), std::move(shortcut), false, radio});
    MENUITEMINFOW item{sizeof item};
    item.fMask = MIIM_FTYPE | MIIM_ID | MIIM_DATA | MIIM_STRING;
    item.fType = MFT_OWNERDRAW | (radio ? MFT_RADIOCHECK : 0) | (column ? MFT_MENUBREAK : 0);
    item.wID = id;
    item.dwItemData = reinterpret_cast<ULONG_PTR>(data.get());
    item.dwTypeData = data->text.data();  // kept for accessibility tools
    InsertMenuItemW(menu, GetMenuItemCount(menu), TRUE, &item);
    m_data.push_back(std::move(data));
}

HMENU MenuBar::AddSubmenu(HMENU menu, std::wstring text) {
    HMENU child = CreatePopup();
    auto data = std::make_unique<Data>(Data{std::move(text), {}, false, false, true});
    MENUITEMINFOW item{sizeof item};
    item.fMask = MIIM_FTYPE | MIIM_SUBMENU | MIIM_DATA | MIIM_STRING;
    item.fType = MFT_OWNERDRAW;
    item.hSubMenu = child;
    item.dwItemData = reinterpret_cast<ULONG_PTR>(data.get());
    item.dwTypeData = data->text.data();
    InsertMenuItemW(menu, GetMenuItemCount(menu), TRUE, &item);
    m_data.push_back(std::move(data));
    return child;
}

void MenuBar::OnMenuSelect(WPARAM wParam, LPARAM lParam) {
    m_activePopup = reinterpret_cast<HMENU>(lParam);
    m_selectionIsSubmenu = HIWORD(wParam) != 0xFFFF && (HIWORD(wParam) & MF_POPUP);
}

void MenuBar::AddSeparator(HMENU menu) {
    auto data = std::make_unique<Data>(Data{{}, {}, true, false});
    MENUITEMINFOW item{sizeof item};
    item.fMask = MIIM_FTYPE | MIIM_DATA;
    item.fType = MFT_OWNERDRAW | MFT_SEPARATOR;
    item.dwItemData = reinterpret_cast<ULONG_PTR>(data.get());
    InsertMenuItemW(menu, GetMenuItemCount(menu), TRUE, &item);
    m_data.push_back(std::move(data));
}

void MenuBar::Remeasure() {
    auto touch = [](HMENU menu) {
        for (int i = 0; i < GetMenuItemCount(menu); ++i) {
            MENUITEMINFOW item{sizeof item};
            item.fMask = MIIM_FTYPE;
            if (GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &item)) SetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &item);
        }
    };
    for (auto& t : m_tops) touch(t.menu.get());
    for (auto& p : m_popups) touch(p.get());
}

void MenuBar::Layout(float left, float top, float height) {
    float x = left;
    for (auto& t : m_tops) {
        const float w = TextWidth(m_fonts, Font::Body, t.label) + m_fonts.F(20);
        t.rect = {x, top, x + w, top + height};
        x += w + m_fonts.F(2);
    }
}

D2D1_RECT_F MenuBar::Bounds() const {
    if (m_tops.empty()) return {};
    return {m_tops.front().rect.left, m_tops.front().rect.top, m_tops.back().rect.right, m_tops.back().rect.bottom};
}

void MenuBar::Paint(Canvas& c) const {
    for (std::size_t i = 0; i < m_tops.size(); ++i) {
        const auto& t = m_tops[i];
        const int index = static_cast<int>(i);
        if (index == m_open) c.Fill(t.rect, Color::Selected, m_fonts.F(4));
        else if (index == m_hot) c.Fill(t.rect, Color::Hover, m_fonts.F(4));
        c.Text(t.label, t.rect, Font::Body, index == m_open || index == m_hot ? Color::Text : Color::Text2, Align::Center);
    }
}

int MenuBar::HitTest(POINT p) const {
    const float x = static_cast<float>(p.x), y = static_cast<float>(p.y);
    for (std::size_t i = 0; i < m_tops.size(); ++i) {
        const auto& r = m_tops[i].rect;
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return static_cast<int>(i);
    }
    return -1;
}

bool MenuBar::SetHot(int index) {
    if (index == m_hot) return false;
    m_hot = index;
    return true;
}

void MenuBar::Invalidate() const {
    if (!m_owner) return;
    const D2D1_RECT_F b = Bounds();
    RECT r{static_cast<LONG>(b.left) - 2, static_cast<LONG>(b.top) - 2, static_cast<LONG>(b.right) + 2, static_cast<LONG>(b.bottom) + 2};
    InvalidateRect(m_owner, &r, FALSE);
    UpdateWindow(m_owner);
}

LRESULT CALLBACK MenuBar::Filter(int code, WPARAM wParam, LPARAM lParam) {
    MenuBar* self = g_tracking;
    if (code == MSGF_MENU && self && self->m_open >= 0) {
        const MSG& msg = *reinterpret_cast<const MSG*>(lParam);
        const int count = static_cast<int>(self->m_tops.size());
        const bool inSubmenu = self->m_activePopup && self->m_activePopup != self->m_tops[static_cast<std::size_t>(self->m_open)].menu.get();
        const bool toSubmenu = msg.wParam == VK_RIGHT && self->m_selectionIsSubmenu;
        const bool fromSubmenu = msg.wParam == VK_LEFT && inSubmenu;
        if (msg.message == WM_KEYDOWN && (msg.wParam == VK_LEFT || msg.wParam == VK_RIGHT) && !toSubmenu && !fromSubmenu) {
            self->m_switchTo = (self->m_open + (msg.wParam == VK_RIGHT ? 1 : count - 1)) % count;
            self->m_switchByKeyboard = true;
            EndMenu();
            return TRUE;
        }
        if (msg.message == WM_MOUSEMOVE || msg.message == WM_LBUTTONDOWN) {
            POINT p = msg.pt;
            ScreenToClient(self->m_owner, &p);
            const int index = self->HitTest(p);
            if (msg.message == WM_MOUSEMOVE && index >= 0 && index != self->m_open) {
                self->m_switchTo = index;
                self->m_switchByKeyboard = false;
                EndMenu();
                return TRUE;
            }
            if (msg.message == WM_LBUTTONDOWN && index == self->m_open) {
                self->m_switchTo = -1;  // clicking the open menu's title closes it
                EndMenu();
                return TRUE;
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void MenuBar::Open(HWND owner, int index, bool keyboard) {
    if (index < 0 || index >= static_cast<int>(m_tops.size()) || g_tracking) return;
    m_owner = owner;
    UINT command = 0;
    while (index >= 0) {
        m_open = index;
        m_switchTo = -1;
        m_activePopup = nullptr;
        m_selectionIsSubmenu = false;
        Invalidate();
        const auto& t = m_tops[static_cast<std::size_t>(index)];
        POINT topLeft{static_cast<LONG>(t.rect.left), static_cast<LONG>(t.rect.top)};
        POINT bottomRight{static_cast<LONG>(t.rect.right), static_cast<LONG>(t.rect.bottom)};
        ClientToScreen(owner, &topLeft);
        ClientToScreen(owner, &bottomRight);
        TPMPARAMS params{sizeof params, {topLeft.x, topLeft.y, bottomRight.x, bottomRight.y}};
        if (keyboard) PostMessageW(owner, WM_KEYDOWN, VK_DOWN, 0);  // highlight the first item
        g_tracking = this;
        {
            const HookHandle hook(SetWindowsHookExW(WH_MSGFILTER, Filter, nullptr, GetCurrentThreadId()));
            command = static_cast<UINT>(TrackPopupMenuEx(t.menu.get(), TPM_LEFTALIGN | TPM_TOPALIGN | TPM_VERTICAL | TPM_RETURNCMD,
                                                         topLeft.x, bottomRight.y, owner, &params));
        }
        g_tracking = nullptr;
        if (command) break;
        index = m_switchTo;
        keyboard = m_switchByKeyboard;
    }
    m_open = -1;
    POINT cursor{};
    GetCursorPos(&cursor);
    ScreenToClient(owner, &cursor);
    m_hot = HitTest(cursor);
    Invalidate();
    if (command) PostMessageW(owner, WM_COMMAND, MAKEWPARAM(command, 0), 0);
}

bool MenuBar::OpenByMnemonic(HWND owner, wchar_t key) {
    key = static_cast<wchar_t>(std::towlower(key));
    for (std::size_t i = 0; i < m_tops.size(); ++i) {
        if (m_tops[i].mnemonic && m_tops[i].mnemonic == key) {
            Open(owner, static_cast<int>(i), true);
            return true;
        }
    }
    return false;
}

void MenuBar::TrackContext(HWND owner, HMENU menu, POINT screen) {
    const UINT command = static_cast<UINT>(TrackPopupMenuEx(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD,
                                                            screen.x, screen.y, owner, nullptr));
    if (command) PostMessageW(owner, WM_COMMAND, MAKEWPARAM(command, 0), 0);
}

void MenuBar::Measure(MEASUREITEMSTRUCT& mis) const {
    const auto* data = reinterpret_cast<const Data*>(mis.itemData);
    if (!data) return;
    if (data->separator) {
        mis.itemWidth = static_cast<UINT>(m_fonts.S(40));
        mis.itemHeight = static_cast<UINT>(m_fonts.S(9));
        return;
    }
    const float text = TextWidth(m_fonts, Font::Body, data->text);
    const float shortcut = data->submenu ? m_fonts.F(28)
                           : data->shortcut.empty() ? 0 : TextWidth(m_fonts, Font::Caption, data->shortcut) + m_fonts.F(32);
    mis.itemWidth = static_cast<UINT>(m_fonts.F(36) + text + shortcut + m_fonts.F(16));
    mis.itemHeight = static_cast<UINT>(m_fonts.S(30));
}

void MenuBar::Draw(const DRAWITEMSTRUCT& dis) const {
    const auto* data = reinterpret_cast<const Data*>(dis.itemData);
    if (!data) return;
    const bool disabled = dis.itemState & (ODS_DISABLED | ODS_GRAYED);
    const bool selected = (dis.itemState & ODS_SELECTED) && !disabled;
    const bool checked = dis.itemState & ODS_CHECKED;
    const auto& f = m_fonts;
    Ui::Paint(dis.hDC, dis.rcItem, f, [&](Canvas& c) {
        const D2D1_RECT_F r = ToF(dis.rcItem);
        c.Clear(Color::Flyout);
        if (data->separator) {
            const float y = std::floor((r.top + r.bottom) / 2) + 0.5f;
            c.Line({r.left + f.F(4), y}, {r.right - f.F(4), y}, Color::FlyoutStroke);
            return;
        }
        if (selected) c.Fill(Inset(r, f.F(4), f.F(2)), Color::FlyoutHot, f.F(4));
        const D2D1_COLOR_F fg = disabled ? Color::TextDisabled : Color::Text;
        if (checked) {
            if (data->radio) {
                c.Circle({r.left + f.F(20), (r.top + r.bottom) / 2}, f.F(3.5f), disabled ? Color::TextDisabled : Color::AccentText);
            } else {
                c.Text(std::wstring(1, Icon::Check), {r.left + f.F(10), r.top, r.left + f.F(30), r.bottom}, Font::IconSmall, fg,
                       Align::Center);
            }
        }
        c.Text(data->text, {r.left + f.F(36), r.top, r.right - f.F(12), r.bottom}, Font::Body, fg);
        if (data->submenu)
            c.Text(std::wstring(1, Icon::ChevronRight), {r.right - f.F(28), r.top, r.right - f.F(10), r.bottom}, Font::IconSmall,
                   disabled ? Color::TextDisabled : Color::Text2, Align::Center);
        else if (!data->shortcut.empty())
            c.Text(data->shortcut, {r.left, r.top, r.right - f.F(16), r.bottom}, Font::Caption,
                   disabled ? Color::TextDisabled : Color::Text3, Align::Right);
    });
    // Windows paints its own (light) submenu arrow after WM_DRAWITEM; keep it out of the item.
    if (data->submenu) ExcludeClipRect(dis.hDC, dis.rcItem.left, dis.rcItem.top, dis.rcItem.right, dis.rcItem.bottom);
}

}  // namespace DL::Ui
