// Menus in the dark palette: a menu bar drawn in the client area (it sits on the Mica backdrop,
// where a native menu bar cannot draw correctly) and owner-drawn popup menus.
//
// Keyboard: Alt+mnemonic or F10 opens a menu, Left/Right switch between menus, Esc closes.
// Mouse: click opens, moving across the bar while a menu is open switches to the menu under it.
#pragma once
#include "gui/Theme.h"
#include "support/Handle.h"
#include <Windows.h>
#include <memory>
#include <string>
#include <vector>

namespace DL::Ui {

class MenuBar {
public:
    explicit MenuBar(const Fonts& fonts);
    ~MenuBar();
    MenuBar(const MenuBar&) = delete;
    MenuBar& operator=(const MenuBar&) = delete;

    // Builders. In a top-level label '&' marks the Alt key ("&File", "Pomo&c").
    HMENU AddMenu(std::wstring label);
    HMENU CreatePopup();  // context menus, owned by the bar as well
    // `column` starts a new column (long lists such as the languages).
    void AddItem(HMENU menu, UINT id, std::wstring text, std::wstring shortcut = {}, bool radio = false, bool column = false);
    HMENU AddSubmenu(HMENU menu, std::wstring text);
    void AddSeparator(HMENU menu);
    void Remeasure();  // after a DPI change (Windows caches owner-draw item sizes)
    void Reset();      // drops every menu (language switch); build them again afterwards

    // Bar in client coordinates.
    void Layout(float left, float top, float height);
    void Paint(Canvas& c) const;
    [[nodiscard]] D2D1_RECT_F Bounds() const;
    [[nodiscard]] int HitTest(POINT client) const;
    bool SetHot(int index);  // true = repaint needed

    // Modal menu loop; the chosen command is posted to `owner` as WM_COMMAND afterwards.
    void Open(HWND owner, int index, bool keyboard);
    bool OpenByMnemonic(HWND owner, wchar_t key);
    void TrackContext(HWND owner, HMENU menu, POINT screen);

    // Owner-draw (forward from the owner's WM_MEASUREITEM / WM_DRAWITEM with CtlType == ODT_MENU).
    void Measure(MEASUREITEMSTRUCT& mis) const;
    void Draw(const DRAWITEMSTRUCT& dis) const;
    // Forward WM_MENUSELECT: Left/Right then open and close submenus instead of switching menus.
    void OnMenuSelect(WPARAM wParam, LPARAM lParam);

private:
    struct Data {
        std::wstring text, shortcut;
        bool separator = false, radio = false, submenu = false;
    };
    struct Top {
        std::wstring label;
        wchar_t mnemonic;
        MenuHandle menu;
        D2D1_RECT_F rect{};
    };
    static LRESULT CALLBACK Filter(int code, WPARAM wParam, LPARAM lParam);
    void Prepare(HMENU menu);
    void Invalidate() const;

    const Fonts& m_fonts;
    Gdi<HBRUSH> m_background;
    std::vector<Top> m_tops;
    std::vector<MenuHandle> m_popups;
    std::vector<std::unique_ptr<Data>> m_data;
    int m_hot = -1, m_open = -1, m_switchTo = -1;
    bool m_switchByKeyboard = false;
    bool m_selectionIsSubmenu = false;  // highlighted item opens a submenu
    HMENU m_activePopup = nullptr;      // popup that has the highlight
    HWND m_owner = nullptr;
};

}  // namespace DL::Ui
