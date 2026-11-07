// Controls drawn with the Direct2D canvas: owner-drawn push buttons, the boot entry list and
// the log view. All of them are real child windows, so Tab, focus and keyboard work as usual.
#pragma once
#include "core/BootList.h"
#include "gui/Theme.h"
#include "support/Common.h"
#include <Windows.h>
#include <optional>
#include <string>
#include <vector>

namespace DL::Ui {

// ---------------------------------------------------------------------------
// Buttons (BS_OWNERDRAW, drawn from the parent's WM_DRAWITEM)
// ---------------------------------------------------------------------------
enum class ButtonKind { Standard, Accent, Combo, Expander, Icon };

struct ButtonLook {
    ButtonKind kind = ButtonKind::Standard;
    wchar_t icon = 0;        // Segoe Fluent Icons code point
    bool onCard = true;      // sits on a card layer (else directly on the window background)
    bool expanded = false;   // Expander
    std::wstring badge;      // Expander: small count next to the label
};

HWND CreateButton(HWND parent, int id, const wchar_t* text);
void DrawButton(const DRAWITEMSTRUCT& dis, const Fonts& fonts, const ButtonLook& look);

// Common icon code points (Segoe Fluent Icons / Segoe MDL2 Assets).
namespace Icon {
inline constexpr wchar_t ChevronDown = 0xE70D, ChevronUp = 0xE70E, ChevronRight = 0xE76C;
inline constexpr wchar_t ArrowUp = 0xE74A, ArrowDown = 0xE74B, Delete = 0xE74D, Add = 0xE710;
inline constexpr wchar_t Play = 0xE768, Pin = 0xE718, Download = 0xE896, Cancel = 0xE711;
inline constexpr wchar_t Folder = 0xE8B7, Check = 0xE73E, Refresh = 0xE72C, Power = 0xE7E8;
}  // namespace Icon

// ---------------------------------------------------------------------------
// Vertical scrolling shared by the list and the log.
// ---------------------------------------------------------------------------
struct Scroller {
    float offset = 0, content = 0, view = 0;
    bool dragging = false, hot = false;
    float grabY = 0, grabOffset = 0;

    [[nodiscard]] float Max() const { return content > view ? content - view : 0; }
    [[nodiscard]] bool Visible() const { return content > view + 0.5f; }
    void Clamp();
    void ScrollTo(float y) { offset = y; Clamp(); }
    void EnsureVisible(float top, float bottom);
    [[nodiscard]] D2D1_RECT_F Track(const RECT& client, const Fonts& f) const;
    [[nodiscard]] D2D1_RECT_F Thumb(const RECT& client, const Fonts& f) const;
    void Draw(Canvas& c, const RECT& client) const;
    bool MouseDown(POINT p, const RECT& client, const Fonts& f);  // true = handled (capture)
    bool MouseMove(POINT p, const RECT& client, const Fonts& f);  // true = offset changed
    bool Wheel(int delta, float step);                              // true = offset changed
};

// ---------------------------------------------------------------------------
// Boot entry list. Notifies the parent with WM_COMMAND (LBN_SELCHANGE) when the selection changes;
// right-click and the menu key raise WM_CONTEXTMENU.
// ---------------------------------------------------------------------------
class EntryList {
public:
    explicit EntryList(const Fonts& fonts) : m_fonts(fonts) {}
    EntryList(const EntryList&) = delete;
    EntryList& operator=(const EntryList&) = delete;

    HWND Create(HWND parent, int id);
    [[nodiscard]] HWND hwnd() const noexcept { return m_hwnd; }

    void SetRows(const Boot::Snapshot& snapshot);  // keeps the selected entry
    void SetPlaceholder(std::wstring text);       // shown while there are no rows
    [[nodiscard]] const Boot::Row* SelectedRow() const;
    void Select(std::uint16_t number);
    void Relayout();  // after a DPI change

private:
    struct Item {
        int row;  // index into m_rows, -1 = group header
        float top, height;
    };
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT, WPARAM, LPARAM);
    void Paint();
    void PaintRow(Canvas& c, const Item& item, const RECT& client);
    int ItemAt(POINT p) const;
    void SelectItem(int item, bool notify);
    void Step(int direction, int count);
    void Rebuild();

    const Fonts& m_fonts;
    HWND m_hwnd = nullptr;
    std::vector<Boot::Row> m_rows;
    std::size_t m_ordered = 0;
    std::vector<Item> m_items;
    int m_selected = -1;  // item index
    int m_hot = -1;
    bool m_tracking = false;
    Scroller m_scroll;
    std::wstring m_placeholder;
};

// ---------------------------------------------------------------------------
// Log view: timestamped, coloured, wrapped lines. Ctrl+C / context menu copy the whole log.
// ---------------------------------------------------------------------------
class LogView {
public:
    explicit LogView(const Fonts& fonts) : m_fonts(fonts) {}
    LogView(const LogView&) = delete;
    LogView& operator=(const LogView&) = delete;

    HWND Create(HWND parent, int id);
    [[nodiscard]] HWND hwnd() const noexcept { return m_hwnd; }

    void Append(Level level, std::wstring text);
    void Clear();
    [[nodiscard]] std::wstring Text() const;
    [[nodiscard]] std::size_t Count() const noexcept { return m_lines.size(); }
    void Relayout();

private:
    struct Line {
        std::wstring time, text;
        Level level;
        float height = 0;  // cached for m_measuredWidth
    };
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT, WPARAM, LPARAM);
    void Measure(Canvas* c);
    void Paint();

    const Fonts& m_fonts;
    HWND m_hwnd = nullptr;
    std::vector<Line> m_lines;
    float m_measuredWidth = -1;
    bool m_follow = true;  // stay at the bottom while new lines arrive
    Scroller m_scroll;
};

// Copies text to the clipboard (CF_UNICODETEXT).
bool CopyToClipboard(HWND owner, const std::wstring& text);

}  // namespace DL::Ui
