// Main window: layout, window placement and painting.
#include "gui/MainWindow.h"
#include "gui/MainWindowDetail.h"
#include "support/Settings.h"
#include "support/Text.h"
#include <algorithm>
#include <cmath>
#include <format>

namespace DL::Ui {
using namespace MainWindowDetail;

int MainWindow::MinClientHeight(bool withLog) const {
    const auto& f = m_fonts;
    // menu + deploy card + boot card with the button column + expander (+ log card)
    const float deploy = kPad + 30 + 6 + 24 + 24 + 10 + 4 + 8 + kButtonH + kPad;
    const float stack = 6 * kButtonH + 3 * kGap + 2 * kGroupGap;
    const float boot = kPad + 20 + 8 + stack + kPad;
    const float total = 4 + kBarH + 6 + deploy + 10 + boot + 6 + 28 + kMargin + (withLog ? kLogH : 0);
    return f.S(static_cast<int>(total));
}

float MainWindow::ButtonWidth() const {
    float widest = m_fonts.F(kButtonW);
    for (UINT id : {CmdCancel, CmdDeploy, CmdUp, CmdDown, CmdOnce, CmdDefault, CmdAdd, CmdDelete}) {
        wchar_t text[128]{};
        GetWindowTextW(GetDlgItem(m_hwnd, static_cast<int>(id)), text, static_cast<int>(std::size(text)));
        const auto look = m_looks.find(id);
        const float icon = look != m_looks.end() && look->second.icon ? m_fonts.F(22) : 0;
        widest = std::max(widest, TextWidth(m_fonts, Font::Body, text) + icon + m_fonts.F(30));
    }
    return std::ceil(widest);
}

void MainWindow::Layout() {
    if (!m_list.hwnd()) return;
    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    const auto& f = m_fonts;
    const float W = static_cast<float>(rc.right), H = static_cast<float>(rc.bottom);
    const float m = f.F(kMargin), p = f.F(kPad), bw = ButtonWidth(), bh = f.F(kButtonH), gap = f.F(kGap);
    auto place = [&](int id, D2D1_RECT_F r) {
        SetWindowPos(GetDlgItem(m_hwnd, id), nullptr, static_cast<int>(std::lround(r.left)), static_cast<int>(std::lround(r.top)),
                     static_cast<int>(std::lround(r.right - r.left)), static_cast<int>(std::lround(r.bottom - r.top)),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    };

    m_menu.Layout(f.F(6), f.F(4), f.F(kBarH));
    float y = f.F(4 + kBarH + 6);

    // Deploy card
    m_deployCard = {m, y, W - m, y};
    float cy = y + p;
    const float left = m + p, right = W - m - p;
    m_labelWidth = f.F(64);
    m_rowSource = {left, cy, right, cy + f.F(30)};
    place(CmdSourceMenu, {left + m_labelWidth, cy, right, cy + f.F(30)});
    cy += f.F(30 + 6);
    m_rowData = {left, cy, right, cy + f.F(24)};
    cy += f.F(24);
    m_rowEsp = {left, cy, right, cy + f.F(24)};
    cy += f.F(24 + 10);
    m_progressRect = {left, cy, right, cy + f.F(4)};
    cy += f.F(4 + 8);
    place(CmdDeploy, {right - bw, cy, right, cy + bh});
    place(CmdCancel, {right - 2 * bw - f.F(8), cy, right - bw - f.F(8), cy + bh});
    m_statusRect = {left, cy, right - 2 * bw - f.F(20), cy + bh};
    cy += bh + p;
    m_deployCard.bottom = cy;

    // Expander and log at the bottom, the boot card takes what is left.
    const float logH = m_logVisible ? std::max(f.F(110), std::min(f.F(kLogH), (H - cy) * 0.45f)) : 0;
    const float expanderH = f.F(28);
    const float expanderTop = H - m - logH - expanderH - (m_logVisible ? f.F(4) : 0);
    place(CmdExpand, {m - f.F(4), expanderTop, m + f.F(160), expanderTop + expanderH});
    if (m_logVisible) {
        m_logCard = {m, expanderTop + expanderH + f.F(4), W - m, H - m};
        place(kIdLog, Inset(m_logCard, f.F(4), f.F(4)));
    }

    y = cy + f.F(10);
    m_bootCard = {m, y, W - m, expanderTop - f.F(6)};
    m_bootHeader = {left, y + p, right, y + p + f.F(20)};
    const float contentTop = m_bootHeader.bottom + f.F(8), contentBottom = m_bootCard.bottom - p;
    place(kIdList, {left - f.F(4), contentTop, right - bw - f.F(10), contentBottom});
    float by = contentTop;
    for (UINT id : {CmdUp, CmdDown, CmdOnce, CmdDefault, CmdAdd, CmdDelete}) {
        place(static_cast<int>(id), {right - bw, by, right, by + bh});
        by += bh + ((id == CmdDown || id == CmdDefault) ? f.F(kGroupGap) : gap);
    }
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void MainWindow::SetLogVisible(bool visible, bool resizeWindow) {
    if (visible == m_logVisible) return;
    m_logVisible = visible;
    Settings::WriteDword(L"LogVisible", visible ? 1 : 0);
    if (visible) m_unread = 0;
    m_looks[CmdExpand].expanded = visible;
    m_looks[CmdExpand].badge.clear();
    ShowWindow(m_log.hwnd(), visible ? SW_SHOW : SW_HIDE);
    if (resizeWindow && !IsZoomed(m_hwnd) && !IsIconic(m_hwnd)) {
        // Grow / shrink the window by the log height (like "More details" in the copy dialog).
        RECT wr{};
        GetWindowRect(m_hwnd, &wr);
        const int delta = m_fonts.S(static_cast<int>(kLogH)) * (visible ? 1 : -1);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        int height = wr.bottom - wr.top + delta;
        int top = wr.top;
        if (top + height > mi.rcWork.bottom) top = std::max<int>(mi.rcWork.top, mi.rcWork.bottom - height);
        height = std::min<int>(height, mi.rcWork.bottom - mi.rcWork.top);
        SetWindowPos(m_hwnd, nullptr, wr.left, top, wr.right - wr.left, height, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    Layout();
    InvalidateRect(GetDlgItem(m_hwnd, CmdExpand), nullptr, FALSE);
}

void MainWindow::RestorePlacement(int showCommand) {
    const UINT dpi = GetDpiForWindow(m_hwnd);
    RECT desired{0, 0, MulDiv(560, static_cast<int>(dpi), 96), MinClientHeight(m_logVisible)};
    AdjustWindowRectExForDpi(&desired, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    int w = desired.right - desired.left, h = desired.bottom - desired.top;
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2;
    int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2;

    // Saved position (physical pixels) when it is still on a monitor.
    int sx = 0, sy = 0, sw = 0, sh = 0;
    const std::wstring saved = Settings::Read(L"WindowRect");
    if (swscanf_s(saved.c_str(), L"%d %d %d %d", &sx, &sy, &sw, &sh) == 4 && sw > 200 && sh > 200) {
        const RECT r{sx, sy, sx + sw, sy + sh};
        if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
            x = sx; y = sy; w = sw; h = sh;
        }
    }
    SetWindowPos(m_hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    Layout();
    ShowWindow(m_hwnd, showCommand == SW_SHOWMINIMIZED || showCommand == SW_MINIMIZE ? SW_SHOWNORMAL : showCommand);
    UpdateWindow(m_hwnd);
}

void MainWindow::SavePlacement() {
    WINDOWPLACEMENT wp{sizeof wp};
    if (!GetWindowPlacement(m_hwnd, &wp)) return;
    const RECT& r = wp.rcNormalPosition;  // workspace coordinates equal screen ones on the primary taskbar layout
    Settings::Write(L"WindowRect", std::format(L"{} {} {} {}", r.left, r.top, r.right - r.left, r.bottom - r.top));
}

void MainWindow::Paint() {
    PaintScope ps(m_hwnd);
    RECT client{};
    GetClientRect(m_hwnd, &client);
    Ui::Paint(ps.dc(), client, m_fonts, [&](Canvas& c) {
        c.Clear(Graphics::Get().Base());
        m_menu.Paint(c);
        PaintDeployCard(c);
        PaintBootCard(c);
        if (m_logVisible) {
            c.Fill(m_logCard, Color::Card, m_fonts.F(6));
            c.Stroke(m_logCard, Color::CardStroke, m_fonts.F(6));
        }
    });
}

void MainWindow::PaintDeployCard(Canvas& c) {
    const auto& f = m_fonts;
    c.Fill(m_deployCard, Color::Card, f.F(6));
    c.Stroke(m_deployCard, Color::CardStroke, f.F(6));

    auto label = [&](const D2D1_RECT_F& row, const std::wstring& text) {
        c.Text(text, {row.left, row.top, row.left + m_labelWidth, row.bottom}, Font::Label, Color::Text3);
    };
    auto value = [&](const D2D1_RECT_F& row, const std::wstring& main, const std::wstring& detail, bool problem) {
        const float x = row.left + m_labelWidth;
        const float mainW = std::min(c.Width(main, Font::Body) + 1, row.right - x);
        c.Text(main, {x, row.top, x + mainW, row.bottom}, Font::Body, problem ? Color::Caution : Color::Text);
        if (!detail.empty()) c.Text(detail, {x + mainW + f.F(10), row.top, row.right, row.bottom}, Font::Caption, Color::Text3);
    };
    label(m_rowSource, Text::Get(IDS_LABEL_SOURCE));
    label(m_rowData, Text::Get(IDS_LABEL_DATA));
    value(m_rowData, m_dataText, m_dataDetail, !m_data);
    label(m_rowEsp, Text::Get(IDS_LABEL_ESP));
    value(m_rowEsp, m_espText, m_espDetail, !m_esp || !m_loaderPresent);

    // Progress: track, determinate fill (animated) or an indeterminate sliding segment.
    const D2D1_RECT_F& t = m_progressRect;
    const float radius = (t.bottom - t.top) / 2;
    c.Fill(t, Color::Track, radius);
    const D2D1_COLOR_F fill = m_failed ? Color::Critical : (m_finished && !m_busy) ? Color::Success : Color::Accent;
    if (m_busy && m_progress.permille < 0) {
        const float width = t.right - t.left, segment = width * 0.28f;
        const float phase = static_cast<float>((GetTickCount64() - m_animStart) % 1600) / 1600.0f;
        const float x = t.left - segment + (width + segment) * phase;
        c.PushClip(t);
        c.Fill({std::max(x, t.left), t.top, std::min(x + segment, t.right), t.bottom}, fill, radius);
        c.PopClip();
    } else if (m_shown > 0.001f) {
        c.Fill({t.left, t.top, t.left + (t.right - t.left) * std::min(m_shown, 1.0f), t.bottom}, fill, radius);
    }

    const D2D1_COLOR_F statusColor = m_statusLevel == Level::Error     ? Color::Critical
                                     : m_statusLevel == Level::Warning ? Color::Caution
                                     : m_statusLevel == Level::Success ? Color::Success
                                                                       : Color::Text2;
    c.Text(m_status, m_statusRect, Font::Caption, statusColor);
}

void MainWindow::PaintBootCard(Canvas& c) {
    const auto& f = m_fonts;
    c.Fill(m_bootCard, Color::Card, f.F(6));
    c.Stroke(m_bootCard, Color::CardStroke, f.F(6));
    c.Text(Text::Get(IDS_BOOT_HEADER), m_bootHeader, Font::Strong, Color::Text);
    std::wstring meta;
    if (!m_uefi) meta = Text::Get(IDS_META_LEGACY);
    else if (!m_nvram) meta = Text::Get(IDS_META_NO_NVRAM);
    else {
        meta = Text::Format(IDS_META, Text::Get(m_secureBoot ? (*m_secureBoot ? IDS_ON : IDS_OFF) : IDS_UNKNOWN), m_snapshot.ordered);
        if (m_snapshot.next) meta += Text::Format(IDS_META_NEXT, Boot::Name(*m_snapshot.next));
    }
    c.Text(meta, m_bootHeader, Font::Caption, Color::Text3, Align::Right);
}

}  // namespace DL::Ui
