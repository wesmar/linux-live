#include "gui/Widgets.h"
#include "support/Text.h"
#include <CommCtrl.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <format>

#pragma comment(lib, "comctl32.lib")

namespace DL::Ui {
namespace {

HINSTANCE Instance() { return GetModuleHandleW(nullptr); }

// ---------------------------------------------------------------------------
// Button hover tracking. Only one control can be under the mouse, so one HWND is enough.
// ---------------------------------------------------------------------------
HWND g_hotButton = nullptr;

LRESULT CALLBACK ButtonProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR) {
    switch (message) {
    case WM_MOUSEMOVE:
        if (g_hotButton != hwnd) {
            if (HWND old = std::exchange(g_hotButton, hwnd)) InvalidateRect(old, nullptr, FALSE);
            TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        if (g_hotButton == hwnd) {
            g_hotButton = nullptr;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    case WM_LBUTTONDBLCLK:
        // Owner-drawn buttons turn a quick second click into BN_DOUBLECLICKED; treat it as a click
        // so "Move up" pressed twice quickly moves twice.
        return DefSubclassProc(hwnd, WM_LBUTTONDOWN, wParam, lParam);
    case WM_ERASEBKGND:
        return 1;
    case WM_ENABLE:
        if (!wParam && g_hotButton == hwnd) g_hotButton = nullptr;
        break;
    case WM_NCDESTROY:
        if (g_hotButton == hwnd) g_hotButton = nullptr;
        RemoveWindowSubclass(hwnd, ButtonProc, id);
        break;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

D2D1_COLOR_F Mix(D2D1_COLOR_F c, float alpha) { return {c.r, c.g, c.b, c.a * alpha}; }

float WheelStep(const Fonts& f, float rowHeight) {
    UINT lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    if (lines == WHEEL_PAGESCROLL) lines = 10;
    return std::max(rowHeight, f.F(18)) * static_cast<float>(std::max(lines, 1u)) / 2.0f;
}

}  // namespace

// ---------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------
HWND CreateButton(HWND parent, int id, const wchar_t* text) {
    HWND b = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), Instance(), nullptr);
    if (b) SetWindowSubclass(b, ButtonProc, 1, 0);
    return b;
}

void DrawButton(const DRAWITEMSTRUCT& dis, const Fonts& f, const ButtonLook& look) {
    const bool disabled = dis.itemState & ODS_DISABLED;
    const bool pressed = (dis.itemState & ODS_SELECTED) && !disabled;
    const bool focus = (dis.itemState & ODS_FOCUS) && !(dis.itemState & ODS_NOFOCUSRECT);
    const bool hot = g_hotButton == dis.hwndItem && !disabled;
    wchar_t label[160]{};
    GetWindowTextW(dis.hwndItem, label, static_cast<int>(std::size(label)));
    const std::wstring_view text(label);
    auto& g = Graphics::Get();

    Paint(dis.hDC, dis.rcItem, f, [&](Canvas& c) {
        const D2D1_RECT_F r = ToF(dis.rcItem);
        c.Clear(look.onCard ? g.Surface(Color::Card) : g.Base());
        const float radius = f.F(4);
        D2D1_COLOR_F fg = disabled ? Color::TextDisabled : pressed ? Color::Text2 : Color::Text;

        switch (look.kind) {
        case ButtonKind::Accent: {
            const D2D1_COLOR_F fill = disabled ? Color::ControlDisabled : pressed ? Color::AccentPressed : hot ? Color::AccentHot : Color::Accent;
            c.Fill(r, fill, radius);
            if (disabled) c.Stroke(r, Color::ControlStroke, radius);
            else fg = pressed ? White(0.8f) : Color::Text;
            break;
        }
        case ButtonKind::Expander:
        case ButtonKind::Icon:
            if (hot || pressed) c.Fill(r, pressed ? Color::ControlPressed : Color::Hover, radius);
            break;
        default:
            c.Fill(r, disabled ? Color::ControlDisabled : pressed ? Color::ControlPressed : hot ? Color::ControlHot : Color::Control, radius);
            c.Stroke(r, Color::ControlStroke, radius);
            break;
        }

        const float pad = f.F(10);
        if (look.kind == ButtonKind::Combo) {
            c.Text(text, {r.left + pad, r.top, r.right - f.F(30), r.bottom}, Font::Body, fg);
            c.Text(std::wstring(1, Icon::ChevronDown), {r.right - f.F(28), r.top, r.right - f.F(8), r.bottom}, Font::IconSmall,
                   disabled ? Color::TextDisabled : Color::Text2, Align::Center);
        } else if (look.kind == ButtonKind::Expander) {
            const wchar_t chevron = look.expanded ? Icon::ChevronDown : Icon::ChevronRight;
            c.Text(std::wstring(1, chevron), {r.left + f.F(4), r.top, r.left + f.F(20), r.bottom}, Font::IconSmall, Color::Text2,
                   Align::Center);
            const float tw = c.Width(text, Font::Strong);
            c.Text(text, {r.left + f.F(24), r.top, r.right, r.bottom}, Font::Strong, fg);
            if (!look.badge.empty()) {
                const float bw = c.Width(look.badge, Font::Label) + f.F(12);
                const D2D1_RECT_F b{r.left + f.F(30) + tw, r.top + f.F(6), r.left + f.F(30) + tw + bw, r.bottom - f.F(6)};
                c.Fill(b, Mix(Color::Accent, 0.9f), (b.bottom - b.top) / 2);
                c.Text(look.badge, b, Font::Label, Color::Text, Align::Center);
            }
        } else if (look.kind == ButtonKind::Icon) {
            c.Text(std::wstring(1, look.icon), r, Font::Icon, fg, Align::Center);
        } else {
            // Icon + label, centred as one group.
            const float iconW = look.icon ? f.F(14) + f.F(8) : 0;
            const float tw = std::min(c.Width(text, Font::Body), r.right - r.left - 2 * pad - iconW);
            float x = r.left + (r.right - r.left - iconW - tw) / 2;
            if (look.icon) {
                c.Text(std::wstring(1, look.icon), {x, r.top, x + f.F(14), r.bottom}, Font::Icon,
                       look.kind == ButtonKind::Accent || disabled ? fg : Color::Text2, Align::Center);
                x += iconW;
            }
            c.Text(text, {x, r.top, x + tw + 1, r.bottom}, Font::Body, fg);
        }
        if (focus) c.Stroke(r, Color::Text, radius, f.F(1.5f));
    });
}

// ---------------------------------------------------------------------------
// Scroller
// ---------------------------------------------------------------------------
void Scroller::Clamp() { offset = std::clamp(offset, 0.0f, Max()); }

void Scroller::EnsureVisible(float top, float bottom) {
    if (top < offset) offset = top;
    else if (bottom > offset + view) offset = bottom - view;
    Clamp();
}

D2D1_RECT_F Scroller::Track(const RECT& client, const Fonts& f) const {
    return {static_cast<float>(client.right) - f.F(9), static_cast<float>(client.top) + f.F(3),
            static_cast<float>(client.right) - f.F(2), static_cast<float>(client.bottom) - f.F(3)};
}

D2D1_RECT_F Scroller::Thumb(const RECT& client, const Fonts& f) const {
    const D2D1_RECT_F t = Track(client, f);
    const float h = t.bottom - t.top;
    const float thumb = std::max(f.F(24), h * (content > 0 ? view / content : 1.0f));
    const float y = t.top + (Max() > 0 ? (h - thumb) * offset / Max() : 0);
    return {t.left, y, t.right, y + thumb};
}

void Scroller::Draw(Canvas& c, const RECT& client) const {
    if (!Visible()) return;
    const auto& f = c.fonts();
    D2D1_RECT_F t = Thumb(client, f);
    const float w = (hot || dragging) ? f.F(6) : f.F(3);
    t.left = t.right - w;
    c.Fill(t, White(hot || dragging ? 0.50f : 0.30f), w / 2);
}

bool Scroller::MouseDown(POINT p, const RECT& client, const Fonts& f) {
    if (!Visible()) return false;
    const D2D1_RECT_F t = Track(client, f);
    if (static_cast<float>(p.x) < t.left - f.F(3)) return false;
    const D2D1_RECT_F thumb = Thumb(client, f);
    const float y = static_cast<float>(p.y);
    if (y < thumb.top) offset -= view;
    else if (y > thumb.bottom) offset += view;
    Clamp();
    dragging = true;
    grabY = y;
    grabOffset = offset;
    return true;
}

bool Scroller::MouseMove(POINT p, const RECT& client, const Fonts& f) {
    if (dragging) {
        const D2D1_RECT_F t = Track(client, f), thumb = Thumb(client, f);
        const float range = (t.bottom - t.top) - (thumb.bottom - thumb.top);
        const float before = offset;
        if (range > 0) offset = grabOffset + (static_cast<float>(p.y) - grabY) * Max() / range;
        Clamp();
        return offset != before;
    }
    const bool now = Visible() && static_cast<float>(p.x) >= Track(client, f).left - f.F(3);
    if (now == hot) return false;
    hot = now;
    return true;
}

bool Scroller::Wheel(int delta, float step) {
    const float before = offset;
    offset -= static_cast<float>(delta) / WHEEL_DELTA * step;
    Clamp();
    return offset != before;
}

// ---------------------------------------------------------------------------
// EntryList
// ---------------------------------------------------------------------------
namespace {
constexpr wchar_t kListClass[] = L"LinuxLive.EntryList";
constexpr wchar_t kLogClass[] = L"LinuxLive.LogView";

void RegisterClassOnce(const wchar_t* name, WNDPROC proc) {
    WNDCLASSEXW wc{sizeof wc};
    if (GetClassInfoExW(Instance(), name, &wc)) return;
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = proc;
    wc.hInstance = Instance();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = name;
    RegisterClassExW(&wc);
}

template <class T>
T* FromWindow(HWND hwnd, UINT message, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* self = static_cast<T*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return self;
    }
    return reinterpret_cast<T*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}
}  // namespace

HWND EntryList::Create(HWND parent, int id) {
    RegisterClassOnce(kListClass, Proc);
    m_hwnd = CreateWindowExW(0, kListClass, L"Boot entries", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), Instance(), this);
    return m_hwnd;
}

LRESULT CALLBACK EntryList::Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = FromWindow<EntryList>(hwnd, message, lParam);
    if (message == WM_NCCREATE && self) self->m_hwnd = hwnd;
    if (message == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (self) self->m_hwnd = nullptr;
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self ? self->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

void EntryList::Rebuild() {
    m_items.clear();
    float y = 0;
    const float row = m_fonts.F(40), header = m_fonts.F(28);
    for (std::size_t i = 0; i < m_rows.size(); ++i) {
        if (i == m_ordered) {
            m_items.push_back({-1, y, header});
            y += header;
        }
        m_items.push_back({static_cast<int>(i), y, row});
        y += row;
    }
    m_scroll.content = y;
    RECT rc{};
    if (m_hwnd) GetClientRect(m_hwnd, &rc);
    m_scroll.view = static_cast<float>(rc.bottom);
    m_scroll.Clamp();
}

void EntryList::Relayout() {
    Rebuild();
    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
}

void EntryList::SetRows(const Boot::Snapshot& snapshot) {
    std::optional<std::uint16_t> keep;
    if (const Boot::Row* r = SelectedRow()) keep = r->entry.number;
    m_rows = snapshot.rows;
    m_ordered = snapshot.ordered;
    m_selected = -1;
    m_hot = -1;
    Rebuild();
    if (keep) Select(*keep);
    if (m_selected < 0) {
        for (std::size_t i = 0; i < m_items.size(); ++i)
            if (m_items[i].row >= 0) { m_selected = static_cast<int>(i); break; }
    }
    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
}

void EntryList::SetPlaceholder(std::wstring text) {
    m_placeholder = std::move(text);
    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
}

const Boot::Row* EntryList::SelectedRow() const {
    if (m_selected < 0 || m_selected >= static_cast<int>(m_items.size())) return nullptr;
    const int row = m_items[static_cast<std::size_t>(m_selected)].row;
    return row >= 0 ? &m_rows[static_cast<std::size_t>(row)] : nullptr;
}

void EntryList::Select(std::uint16_t number) {
    for (std::size_t i = 0; i < m_items.size(); ++i) {
        const int row = m_items[i].row;
        if (row >= 0 && m_rows[static_cast<std::size_t>(row)].entry.number == number) {
            SelectItem(static_cast<int>(i), false);
            return;
        }
    }
}

void EntryList::SelectItem(int item, bool notify) {
    if (item < 0 || item >= static_cast<int>(m_items.size()) || m_items[static_cast<std::size_t>(item)].row < 0) return;
    const bool changed = item != m_selected;
    m_selected = item;
    const Item& it = m_items[static_cast<std::size_t>(item)];
    m_scroll.EnsureVisible(it.top, it.top + it.height);
    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
    if (changed && notify && m_hwnd)
        SendMessageW(GetParent(m_hwnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(m_hwnd), LBN_SELCHANGE),
                     reinterpret_cast<LPARAM>(m_hwnd));
}

void EntryList::Step(int direction, int count) {
    int i = m_selected < 0 ? (direction > 0 ? -1 : static_cast<int>(m_items.size())) : m_selected;
    int target = m_selected;
    for (int moved = 0; moved < count;) {
        i += direction;
        if (i < 0 || i >= static_cast<int>(m_items.size())) break;
        if (m_items[static_cast<std::size_t>(i)].row < 0) continue;
        target = i;
        ++moved;
    }
    SelectItem(target, true);
}

int EntryList::ItemAt(POINT p) const {
    const float y = static_cast<float>(p.y) + m_scroll.offset;
    for (std::size_t i = 0; i < m_items.size(); ++i)
        if (y >= m_items[i].top && y < m_items[i].top + m_items[i].height) return static_cast<int>(i);
    return -1;
}

LRESULT EntryList::Handle(UINT message, WPARAM wParam, LPARAM lParam) {
    RECT client{};
    GetClientRect(m_hwnd, &client);
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        Paint();
        return 0;
    case WM_SIZE:
        Rebuild();
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    case WM_KEYDOWN: {
        const int page = std::max(1, static_cast<int>(m_scroll.view / m_fonts.F(40)) - 1);
        switch (wParam) {
        case VK_UP: Step(-1, 1); return 0;
        case VK_DOWN: Step(1, 1); return 0;
        case VK_PRIOR: Step(-1, page); return 0;
        case VK_NEXT: Step(1, page); return 0;
        case VK_HOME: Step(-1, 1 << 20); return 0;
        case VK_END: Step(1, 1 << 20); return 0;
        }
        break;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: {
        SetFocus(m_hwnd);
        const POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (message != WM_RBUTTONDOWN && m_scroll.MouseDown(p, client, m_fonts)) {
            SetCapture(m_hwnd);
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return 0;
        }
        SelectItem(ItemAt(p), true);
        if (message == WM_LBUTTONDBLCLK)
            SendMessageW(GetParent(m_hwnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(m_hwnd), LBN_DBLCLK),
                         reinterpret_cast<LPARAM>(m_hwnd));
        return 0;
    }
    case WM_MOUSEMOVE: {
        const POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (!m_tracking) {
            TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, m_hwnd, 0};
            m_tracking = TrackMouseEvent(&tme) != FALSE;
        }
        bool repaint = m_scroll.MouseMove(p, client, m_fonts);
        const int hot = m_scroll.dragging || m_scroll.hot ? -1 : ItemAt(p);
        if (hot != m_hot) { m_hot = hot; repaint = true; }
        if (repaint) InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP:
        if (m_scroll.dragging) {
            m_scroll.dragging = false;
            ReleaseCapture();
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_CAPTURECHANGED:
        m_scroll.dragging = false;
        return 0;
    case WM_MOUSELEAVE:
        m_tracking = false;
        m_hot = -1;
        m_scroll.hot = false;
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    case WM_MOUSEWHEEL:
        if (m_scroll.Wheel(GET_WHEEL_DELTA_WPARAM(wParam), WheelStep(m_fonts, m_fonts.F(40)))) InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    }
    return DefWindowProcW(m_hwnd, message, wParam, lParam);
}

void EntryList::PaintRow(Canvas& c, const Item& item, const RECT& client) {
    const auto& f = m_fonts;
    const float top = item.top - m_scroll.offset, bottom = top + item.height;
    const float right = static_cast<float>(client.right) - (m_scroll.Visible() ? f.F(12) : f.F(4));
    if (item.row < 0) {
        c.Text(Text::Get(IDS_LIST_UNLISTED), {f.F(12), top + f.F(6), right, bottom}, Font::Label, Color::Text3);
        return;
    }
    const Boot::Row& r = m_rows[static_cast<std::size_t>(item.row)];
    const int index = static_cast<int>(&item - m_items.data());
    const bool selected = index == m_selected, hot = index == m_hot;
    const D2D1_RECT_F box{f.F(4), top + f.F(1), right, bottom - f.F(1)};
    if (selected) {
        c.Fill(box, Color::Selected, f.F(4));
        const float mid = (box.top + box.bottom) / 2;
        c.Fill({box.left + f.F(2), mid - f.F(8), box.left + f.F(5), mid + f.F(8)}, Color::Accent, f.F(1.5f));
        if (GetFocus() == m_hwnd) c.Stroke(box, White(0.22f), f.F(4));
    } else if (hot) {
        c.Fill(box, Color::Hover, f.F(4));
    }

    const bool active = r.entry.Active();
    const D2D1_COLOR_F main = active ? Color::Text : Color::TextDisabled;
    const std::wstring position = r.position >= 0 ? std::to_wstring(r.position + 1) : std::wstring(L"\x2013");
    c.Text(position, {f.F(10), top, f.F(34), top + f.F(24)}, Font::Caption, Color::Text3, Align::Center);

    // Badges first (right side of line one), the name takes what is left.
    struct Badge {
        std::wstring text;
        D2D1_COLOR_F color;
    };
    std::vector<Badge> badges;
    if (r.current) badges.push_back({Text::Get(IDS_BADGE_CURRENT), Color::Success});
    if (r.next) badges.push_back({Text::Get(IDS_BADGE_NEXT), Color::AccentText});
    if (r.position == 0) badges.push_back({Text::Get(IDS_BADGE_DEFAULT), Color::Text2});
    if (!active) badges.push_back({Text::Get(IDS_BADGE_OFF), Color::Text3});
    if (r.entry.Application()) badges.push_back({Text::Get(IDS_BADGE_APP), Color::Text3});
    if (r.linux) badges.push_back({Text::Get(IDS_BADGE_DEBIAN), Color::AccentText});
    float badgesWidth = 0;
    std::vector<float> widths;
    for (const auto& b : badges) {
        widths.push_back(c.Width(b.text, Font::Label) + f.F(12));
        badgesWidth += widths.back() + f.F(4);
    }
    const float x0 = f.F(40);
    const float nameMax = std::max(f.F(40), right - x0 - badgesWidth - f.F(10));
    const float nameW = std::min(c.Width(r.entry.description, Font::Body) + 1, nameMax);
    c.Text(r.entry.description, {x0, top + f.F(3), x0 + nameW, top + f.F(22)}, Font::Body, main);
    float bx = x0 + nameW + f.F(8);
    for (std::size_t i = 0; i < badges.size() && bx + widths[i] <= right; ++i) {
        const D2D1_RECT_F b{bx, top + f.F(5), bx + widths[i], top + f.F(20)};
        c.Fill(b, Mix(badges[i].color, 0.16f), f.F(3));
        c.Text(badges[i].text, b, Font::Label, badges[i].color, Align::Center);
        bx += widths[i] + f.F(4);
    }
    const std::wstring detail = std::format(L"{}   {}", Boot::Name(r.entry.number), Boot::Target(r.entry));
    c.Text(detail, {x0, top + f.F(21), right - f.F(4), top + f.F(37)}, Font::Caption, active ? Color::Text3 : Color::TextDisabled);
}

void EntryList::Paint() {
    PaintScope ps(m_hwnd);
    RECT client{};
    GetClientRect(m_hwnd, &client);
    m_scroll.view = static_cast<float>(client.bottom);
    m_scroll.Clamp();
    Ui::Paint(ps.dc(), client, m_fonts, [&](Canvas& c) {
        c.Clear(Graphics::Get().Surface(Color::Card));
        if (m_items.empty()) {
            c.Text(m_placeholder, ToF(client), Font::Body, Color::Text3, Align::Center);
            return;
        }
        c.PushClip(ToF(client));
        for (const Item& item : m_items) {
            const float top = item.top - m_scroll.offset;
            if (top + item.height < 0 || top > static_cast<float>(client.bottom)) continue;
            PaintRow(c, item, client);
        }
        c.PopClip();
        m_scroll.Draw(c, client);
    });
}

// ---------------------------------------------------------------------------
// LogView
// ---------------------------------------------------------------------------
HWND LogView::Create(HWND parent, int id) {
    RegisterClassOnce(kLogClass, Proc);
    m_hwnd = CreateWindowExW(0, kLogClass, L"Log", WS_CHILD | WS_TABSTOP, 0, 0, 10, 10, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), Instance(), this);
    return m_hwnd;
}

LRESULT CALLBACK LogView::Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = FromWindow<LogView>(hwnd, message, lParam);
    if (message == WM_NCCREATE && self) self->m_hwnd = hwnd;
    if (message == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (self) self->m_hwnd = nullptr;
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self ? self->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

void LogView::Append(Level level, std::wstring text) {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    constexpr std::size_t kMaxLines = 5000;
    if (m_lines.size() >= kMaxLines) m_lines.erase(m_lines.begin(), m_lines.begin() + kMaxLines / 10);
    m_lines.push_back({std::format(L"{:02}:{:02}:{:02}", t.wHour, t.wMinute, t.wSecond), std::move(text), level});
    if (m_hwnd && IsWindowVisible(m_hwnd)) InvalidateRect(m_hwnd, nullptr, FALSE);
}

void LogView::Clear() {
    m_lines.clear();
    m_scroll = {};
    m_follow = true;
    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
}

std::wstring LogView::Text() const {
    std::wstring out;
    for (const auto& l : m_lines) out += std::format(L"{}  {}\r\n", l.time, l.text);
    return out;
}

void LogView::Relayout() {
    m_measuredWidth = -1;
    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
}

void LogView::Measure(Canvas* c) {
    RECT client{};
    GetClientRect(m_hwnd, &client);
    const float width = static_cast<float>(client.right) - m_fonts.F(84);
    const bool all = width != m_measuredWidth;
    m_measuredWidth = width;
    float total = m_fonts.F(6);
    const float minimum = m_fonts.F(17);
    for (auto& l : m_lines) {
        if (all || l.height <= 0) l.height = std::max(minimum, c->ParagraphHeight(l.text, width, Font::Mono) + m_fonts.F(2));
        total += l.height;
    }
    m_scroll.content = total + m_fonts.F(6);
    m_scroll.view = static_cast<float>(client.bottom);
    if (m_follow) m_scroll.offset = m_scroll.Max();
    m_scroll.Clamp();
}

void LogView::Paint() {
    PaintScope ps(m_hwnd);
    RECT client{};
    GetClientRect(m_hwnd, &client);
    Ui::Paint(ps.dc(), client, m_fonts, [&](Canvas& c) {
        c.Clear(Graphics::Get().Surface(Color::Card));
        Measure(&c);
        if (m_lines.empty()) {
            c.Text(Text::Get(IDS_LOG_EMPTY), ToF(client), Font::Caption, Color::Text3, Align::Center);
            return;
        }
        const auto& f = m_fonts;
        const float right = static_cast<float>(client.right) - f.F(14);
        float y = f.F(6) - m_scroll.offset;
        c.PushClip(ToF(client));
        for (const auto& l : m_lines) {
            if (y + l.height >= 0 && y <= static_cast<float>(client.bottom)) {
                const D2D1_COLOR_F color = l.level == Level::Error     ? Color::Critical
                                           : l.level == Level::Warning ? Color::Caution
                                           : l.level == Level::Success ? Color::Success
                                                                       : Color::Text2;
                c.Text(l.time, {f.F(10), y, f.F(70), y + f.F(17)}, Font::Mono, Color::Text3);
                c.Paragraph(l.text, {f.F(70), y + f.F(1), right, y + l.height}, Font::Mono, color);
            }
            y += l.height;
        }
        c.PopClip();
        m_scroll.Draw(c, client);
    });
}

LRESULT LogView::Handle(UINT message, WPARAM wParam, LPARAM lParam) {
    RECT client{};
    GetClientRect(m_hwnd, &client);
    auto changed = [&] {
        m_follow = m_scroll.offset >= m_scroll.Max() - 1;
        InvalidateRect(m_hwnd, nullptr, FALSE);
    };
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        Paint();
        return 0;
    case WM_SIZE:
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_KEYDOWN: {
        const float line = m_fonts.F(17);
        if (wParam == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            CopyToClipboard(m_hwnd, Text());
            return 0;
        }
        const float before = m_scroll.offset;
        switch (wParam) {
        case VK_UP: m_scroll.ScrollTo(m_scroll.offset - line); break;
        case VK_DOWN: m_scroll.ScrollTo(m_scroll.offset + line); break;
        case VK_PRIOR: m_scroll.ScrollTo(m_scroll.offset - m_scroll.view); break;
        case VK_NEXT: m_scroll.ScrollTo(m_scroll.offset + m_scroll.view); break;
        case VK_HOME: m_scroll.ScrollTo(0); break;
        case VK_END: m_scroll.ScrollTo(m_scroll.Max()); break;
        default: return DefWindowProcW(m_hwnd, message, wParam, lParam);
        }
        if (m_scroll.offset != before) changed();
        return 0;
    }
    case WM_LBUTTONDOWN: {
        SetFocus(m_hwnd);
        const POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (m_scroll.MouseDown(p, client, m_fonts)) {
            SetCapture(m_hwnd);
            changed();
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, m_hwnd, 0};
        TrackMouseEvent(&tme);
        if (m_scroll.MouseMove({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}, client, m_fonts)) changed();
        return 0;
    }
    case WM_MOUSELEAVE:
        if (m_scroll.hot) {
            m_scroll.hot = false;
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (m_scroll.dragging) {
            m_scroll.dragging = false;
            ReleaseCapture();
            changed();
        }
        return 0;
    case WM_CAPTURECHANGED:
        m_scroll.dragging = false;
        return 0;
    case WM_MOUSEWHEEL:
        if (m_scroll.Wheel(GET_WHEEL_DELTA_WPARAM(wParam), WheelStep(m_fonts, m_fonts.F(17)))) changed();
        return 0;
    }
    return DefWindowProcW(m_hwnd, message, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Clipboard
// ---------------------------------------------------------------------------
bool CopyToClipboard(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    const ScopeExit close([] { CloseClipboard(); });
    EmptyClipboard();
    const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    struct GlobalFreer {
        void operator()(void* h) const noexcept { GlobalFree(h); }
    };
    std::unique_ptr<void, GlobalFreer> memory(GlobalAlloc(GMEM_MOVEABLE, bytes));
    if (!memory) return false;
    if (void* p = GlobalLock(memory.get())) {
        std::memcpy(p, text.c_str(), bytes);
        GlobalUnlock(memory.get());
    } else {
        return false;
    }
    if (!SetClipboardData(CF_UNICODETEXT, memory.get())) return false;
    memory.release();  // owned by the clipboard now
    return true;
}

}  // namespace DL::Ui
