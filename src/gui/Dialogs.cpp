#include "gui/Dialogs.h"
#include "gui/Theme.h"
#include "gui/Widgets.h"
#include "support/Config.h"
#include "support/Handle.h"
#include "support/Text.h"
#include <CommCtrl.h>
#include <ShObjIdl.h>
#include <uxtheme.h>
#include <algorithm>
#include <unordered_map>

#pragma comment(lib, "ole32.lib")

namespace DL::Ui {
namespace {

constexpr wchar_t kClass[] = L"LinuxLive.Dialog";
constexpr int kExtra = 3, kFieldBase = 100, kBrowseBase = 200, kMonospace = 300;

Gdi<HFONT> MakeGdiFont(const Fonts& f, const wchar_t* family, float pixels) {
    return Gdi<HFONT>(CreateFontW(-static_cast<int>(f.F(pixels) + 0.5f), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, family));
}

std::optional<std::wstring> BrowseFile(HWND owner, const wchar_t* filter, const std::wstring& current) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.GetAddressOf()))))
        return std::nullopt;
    std::vector<COMDLG_FILTERSPEC> specs;
    for (const wchar_t* p = filter; p && *p;) {
        const wchar_t* name = p;
        p += wcslen(p) + 1;
        const wchar_t* pattern = p;
        p += wcslen(p) + 1;
        specs.push_back({name, pattern});
    }
    if (!specs.empty()) dialog->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
    if (!current.empty() && current.find(L"://") == std::wstring::npos) {
        ComPtr<IShellItem> folder;
        const std::wstring dir = current.substr(0, current.find_last_of(L"\\/"));
        if (SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(folder.GetAddressOf()))))
            dialog->SetFolder(folder.Get());
    }
    if (FAILED(dialog->Show(owner))) return std::nullopt;
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.GetAddressOf()))) return std::nullopt;
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return std::nullopt;
    std::wstring result(path);
    CoTaskMemFree(path);
    return result;
}

class Dialog {
public:
    Dialog(HWND owner, const DialogSpec& spec) : m_owner(owner), m_spec(spec) {}

    DialogResult Run() {
        RegisterOnce();
        m_fonts.Create(GetDpiForWindow(m_owner));
        m_hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, Config::kAppTitle, WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                                 0, 0, 100, 100, m_owner, nullptr, GetModuleHandleW(nullptr), this);
        if (!m_hwnd) return {};
        const bool ownerWasEnabled = !EnableWindow(m_owner, FALSE);
        ShowWindow(m_hwnd, SW_SHOW);
        if (!m_edits.empty()) {
            SetFocus(m_edits.front());
            SendMessageW(m_edits.front(), EM_SETSEL, 0, -1);
        } else {
            SetFocus(GetDlgItem(m_hwnd, m_spec.cancelButton ? IDCANCEL : IDOK));
        }
        MSG msg;
        while (!m_done) {
            const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
            if (got <= 0) {
                if (got == 0) PostQuitMessage(static_cast<int>(msg.wParam));
                break;
            }
            if (!IsDialogMessageW(m_hwnd, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        if (ownerWasEnabled) EnableWindow(m_owner, TRUE);  // before destroying, so activation returns to the owner
        DestroyWindow(m_hwnd);
        return m_result;
    }

private:
    static void RegisterOnce() {
        WNDCLASSEXW wc{sizeof wc};
        if (GetClassInfoExW(GetModuleHandleW(nullptr), kClass, &wc)) return;
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
    }

    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        Dialog* self = nullptr;
        if (message == WM_NCCREATE) {
            self = static_cast<Dialog*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            self->m_hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<Dialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        if (message == WM_NCDESTROY) SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return self ? self->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    void Build() {
        ApplyBackdrop(m_hwnd);
        auto& g = Graphics::Get();
        m_fieldBrush.reset(CreateSolidBrush(g.GdiSurface(Color::Control)));
        for (std::size_t i = 0; i < m_spec.fields.size(); ++i) {
            const auto& field = m_spec.fields[i];
            HWND edit = CreateWindowExW(0, L"EDIT", field.value.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10,
                                        10, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFieldBase + i)), nullptr, nullptr);
            if (!field.cue.empty()) SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(field.cue.c_str()));
            SendMessageW(edit, EM_SETLIMITTEXT, 4096, 0);
            m_edits.push_back(edit);
            HWND browse = nullptr;
            if (field.filter) {
                browse = CreateButton(m_hwnd, static_cast<int>(kBrowseBase + i), Text::Get(IDS_BROWSE).c_str());
                m_looks[browse] = {ButtonKind::Icon, Icon::Folder, false};
            }
            m_browse.push_back(browse);
        }
        if (!m_spec.monospace.empty()) {
            m_mono = CreateWindowExW(0, L"EDIT", nullptr,
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0, 0,
                                     10, 10, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kMonospace)), nullptr, nullptr);
            SetWindowTheme(m_mono, L"DarkMode_Explorer", nullptr);
            std::wstring text;
            for (wchar_t c : m_spec.monospace) {  // the EDIT control wants CRLF
                if (c == L'\n') text += L'\r';
                text += c;
            }
            SetWindowTextW(m_mono, text.c_str());
        }
        auto button = [&](int id, const std::wstring& text, ButtonKind kind) {
            HWND b = CreateButton(m_hwnd, id, text.c_str());
            m_looks[b] = {kind, 0, false};
            m_buttons.push_back(b);
        };
        if (!m_spec.extra.empty()) button(kExtra, m_spec.extra, ButtonKind::Standard);
        button(IDOK, m_spec.ok.empty() ? Text::Get(IDS_OK) : m_spec.ok, ButtonKind::Accent);
        if (m_spec.cancelButton) button(IDCANCEL, m_spec.cancel.empty() ? Text::Get(IDS_CANCEL) : m_spec.cancel, ButtonKind::Standard);
        Layout();
    }

    void Layout() {
        const auto& f = m_fonts;
        m_editFont = MakeGdiFont(f, f.UiFamily(), 13.0f);
        m_monoFont = MakeGdiFont(f, Graphics::Get().HasFamily(L"Cascadia Mono") ? L"Cascadia Mono" : L"Consolas", 11.5f);
        const float width = f.F(static_cast<float>(m_spec.width));
        const float pad = f.F(24), inner = width - 2 * pad;
        float y = pad;
        m_headingRect = {pad, y, width - pad, y + f.F(28)};
        y += f.F(36);
        if (!m_spec.message.empty()) {
            const float h = TextHeight(f, Font::Body, m_spec.message, inner);
            m_messageRect = {pad, y, width - pad, y + h};
            y += h + f.F(16);
        }
        m_fieldRects.clear();
        m_labelRects.clear();
        for (std::size_t i = 0; i < m_edits.size(); ++i) {
            m_labelRects.push_back({pad, y, width - pad, y + f.F(18)});
            y += f.F(22);
            const float browseW = m_browse[i] ? f.F(36) : 0;
            const D2D1_RECT_F box{pad, y, width - pad - (browseW ? browseW + f.F(6) : 0), y + f.F(32)};
            m_fieldRects.push_back(box);
            const int lineH = f.S(18);
            SetWindowPos(m_edits[i], nullptr, static_cast<int>(box.left + f.F(10)), static_cast<int>((box.top + box.bottom) / 2) - lineH / 2,
                         static_cast<int>(box.right - box.left - f.F(20)), lineH, SWP_NOZORDER | SWP_NOACTIVATE);
            SendMessageW(m_edits[i], WM_SETFONT, reinterpret_cast<WPARAM>(m_editFont.get()), TRUE);
            if (m_browse[i])
                SetWindowPos(m_browse[i], nullptr, static_cast<int>(width - pad - browseW), static_cast<int>(box.top),
                             static_cast<int>(browseW), static_cast<int>(f.F(32)), SWP_NOZORDER | SWP_NOACTIVATE);
            y += f.F(32) + f.F(12);
        }
        if (m_mono) {
            const D2D1_RECT_F box{pad, y, width - pad, y + f.F(380)};
            m_monoRect = box;
            SetWindowPos(m_mono, nullptr, static_cast<int>(box.left + f.F(8)), static_cast<int>(box.top + f.F(6)),
                         static_cast<int>(box.right - box.left - f.F(10)), static_cast<int>(box.bottom - box.top - f.F(12)),
                         SWP_NOZORDER | SWP_NOACTIVATE);
            SendMessageW(m_mono, WM_SETFONT, reinterpret_cast<WPARAM>(m_monoFont.get()), TRUE);
            y = box.bottom + f.F(12);
        }
        m_errorRect = {pad, y, width - pad, y + f.F(20)};
        y += f.F(24);
        // Footer with the buttons, right-aligned, all the same width.
        const float footer = f.F(72);
        m_footer = {0, y, width, y + footer};
        const float bw = f.F(124), bh = f.F(32);
        float x = width - pad;
        for (auto it = m_buttons.rbegin(); it != m_buttons.rend(); ++it) {
            x -= bw;
            SetWindowPos(*it, nullptr, static_cast<int>(x), static_cast<int>(y + (footer - bh) / 2), static_cast<int>(bw),
                         static_cast<int>(bh), SWP_NOZORDER | SWP_NOACTIVATE);
            x -= f.F(8);
        }
        RECT rc{0, 0, static_cast<LONG>(width), static_cast<LONG>(y + footer)};
        AdjustWindowRectExForDpi(&rc, static_cast<DWORD>(GetWindowLongPtrW(m_hwnd, GWL_STYLE)), FALSE,
                                 static_cast<DWORD>(GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE)), f.dpi());
        RECT owner{};
        GetWindowRect(m_owner, &owner);
        const int w = rc.right - rc.left, h = rc.bottom - rc.top;
        SetWindowPos(m_hwnd, nullptr, owner.left + (owner.right - owner.left - w) / 2, owner.top + (owner.bottom - owner.top - h) / 3, w,
                     h, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void PaintSurface() {
        PaintScope ps(m_hwnd);
        RECT client{};
        GetClientRect(m_hwnd, &client);
        const auto& f = m_fonts;
        HWND focus = GetFocus();
        Paint(
            ps.dc(), client, f,
            [&](Canvas& c) {
                c.Clear(Graphics::Get().Base());
                c.Text(m_spec.heading, m_headingRect, Font::Title, m_spec.danger ? Color::Critical : Color::Text);
                if (!m_spec.message.empty()) c.Paragraph(m_spec.message, m_messageRect, Font::Body, Color::Text2);
                for (std::size_t i = 0; i < m_fieldRects.size(); ++i) {
                    c.Text(m_spec.fields[i].label, m_labelRects[i], Font::Caption, Color::Text2);
                    const D2D1_RECT_F& box = m_fieldRects[i];
                    c.Fill(box, Color::Control, f.F(4));
                    c.Stroke(box, Color::ControlStroke, f.F(4));
                    const bool focused = focus == m_edits[i];
                    c.Fill({box.left + f.F(1), box.bottom - f.F(focused ? 2.0f : 1.0f), box.right - f.F(1), box.bottom},
                           focused ? Color::Accent : White(0.30f));
                }
                if (m_mono) {
                    c.Fill(m_monoRect, Color::Control, f.F(4));
                    c.Stroke(m_monoRect, Color::ControlStroke, f.F(4));
                }
                if (!m_error.empty()) c.Text(m_error, m_errorRect, Font::Caption, Color::Critical);
                c.Fill(m_footer, Black(0.18f));
                c.Line({m_footer.left, m_footer.top + 0.5f}, {m_footer.right, m_footer.top + 0.5f}, White(0.06f));
            },
            [&](HDC dc) {
                // The EDIT controls paint with GDI; the padding around them is filled the same way so
                // field and control are one surface.
                for (const auto& box : m_fieldRects) {
                    const RECT r{static_cast<LONG>(box.left + f.F(3)), static_cast<LONG>(box.top + f.F(3)),
                                 static_cast<LONG>(box.right - f.F(3)), static_cast<LONG>(box.bottom - f.F(3))};
                    FillRect(dc, &r, m_fieldBrush.get());
                }
                if (m_mono) {
                    const RECT r{static_cast<LONG>(m_monoRect.left + f.F(3)), static_cast<LONG>(m_monoRect.top + f.F(3)),
                                 static_cast<LONG>(m_monoRect.right - f.F(3)), static_cast<LONG>(m_monoRect.bottom - f.F(3))};
                    FillRect(dc, &r, m_fieldBrush.get());
                }
            });
    }

    void Finish(int button) {
        std::vector<std::wstring> values;
        for (HWND e : m_edits) {
            const int n = GetWindowTextLengthW(e);
            std::wstring v(static_cast<std::size_t>(n) + 1, L'\0');
            GetWindowTextW(e, v.data(), n + 1);
            v.resize(static_cast<std::size_t>(n));
            // Trim surrounding blanks and quotes (paths pasted from Explorer's "Copy as path").
            const auto first = v.find_first_not_of(L" \t\"");
            const auto last = v.find_last_not_of(L" \t\"");
            values.push_back(first == std::wstring::npos ? std::wstring() : v.substr(first, last - first + 1));
        }
        if (button == 1 && m_spec.validate) {
            if (auto error = m_spec.validate(values)) {
                m_error = *error;
                InvalidateRect(m_hwnd, nullptr, FALSE);
                return;
            }
        }
        m_result = {button, std::move(values)};
        m_done = true;
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE:
            Build();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            PaintSurface();
            return 0;
        case WM_DPICHANGED:
            m_fonts.Create(HIWORD(wParam));
            Layout();
            InvalidateRect(m_hwnd, nullptr, TRUE);
            return 0;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, ToColorRef(Color::Text));
            SetBkColor(dc, Graphics::Get().GdiSurface(Color::Control));
            return reinterpret_cast<LRESULT>(m_fieldBrush.get());
        }
        case WM_DRAWITEM: {
            const auto& dis = *reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (auto it = m_looks.find(dis.hwndItem); it != m_looks.end()) DrawButton(dis, m_fonts, it->second);
            return TRUE;
        }
        case DM_GETDEFID:
            return MAKELRESULT(IDOK, DC_HASDEFID);
        case WM_COMMAND: {
            const int id = LOWORD(wParam), code = HIWORD(wParam);
            if (id >= kFieldBase && id < kFieldBase + 100 && (code == EN_SETFOCUS || code == EN_KILLFOCUS)) {
                InvalidateRect(m_hwnd, nullptr, FALSE);
                return 0;
            }
            if (id >= kBrowseBase && id < kBrowseBase + 100) {
                const std::size_t i = static_cast<std::size_t>(id - kBrowseBase);
                wchar_t current[4096]{};
                GetWindowTextW(m_edits[i], current, 4096);
                if (auto path = BrowseFile(m_hwnd, m_spec.fields[i].filter, current)) SetWindowTextW(m_edits[i], path->c_str());
                return 0;
            }
            if (id == IDOK) Finish(1);
            else if (id == IDCANCEL) Finish(m_spec.cancelButton ? 0 : 1);
            else if (id == kExtra) Finish(2);
            return 0;
        }
        case WM_CLOSE:
            Finish(m_spec.cancelButton ? 0 : 1);
            return 0;
        }
        return DefWindowProcW(m_hwnd, message, wParam, lParam);
    }

    HWND m_owner;
    const DialogSpec& m_spec;
    HWND m_hwnd = nullptr;
    Fonts m_fonts;
    Gdi<HFONT> m_editFont, m_monoFont;
    Gdi<HBRUSH> m_fieldBrush;
    std::vector<HWND> m_edits, m_browse, m_buttons;
    HWND m_mono = nullptr;
    std::unordered_map<HWND, ButtonLook> m_looks;
    D2D1_RECT_F m_headingRect{}, m_messageRect{}, m_monoRect{}, m_errorRect{}, m_footer{};
    std::vector<D2D1_RECT_F> m_fieldRects, m_labelRects;
    std::wstring m_error;
    DialogResult m_result;
    bool m_done = false;
};

}  // namespace

DialogResult RunDialog(HWND owner, const DialogSpec& spec) {
    Dialog dialog(owner, spec);
    return dialog.Run();
}

bool Confirm(HWND owner, std::wstring heading, std::wstring message, std::wstring ok, bool danger) {
    DialogSpec spec;
    spec.heading = std::move(heading);
    spec.message = std::move(message);
    spec.ok = std::move(ok);
    spec.danger = danger;
    return RunDialog(owner, spec).button == 1;
}

void Inform(HWND owner, std::wstring heading, std::wstring message) {
    DialogSpec spec;
    spec.heading = std::move(heading);
    spec.message = std::move(message);
    spec.cancelButton = false;
    RunDialog(owner, spec);
}

}  // namespace DL::Ui
