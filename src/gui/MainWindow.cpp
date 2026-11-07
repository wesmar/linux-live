// Main window: creation, message loop and window procedure.
#include "gui/MainWindow.h"
#include "gui/MainWindowDetail.h"
#include "core/Actions.h"
#include "core/Sources.h"
#include "gui/Dialogs.h"
#include "support/Config.h"
#include "support/Settings.h"
#include "support/Text.h"
#include <windowsx.h>
#include <algorithm>
#include <cmath>

namespace DL::Ui {
using namespace MainWindowDetail;

MainWindow::MainWindow(HINSTANCE instance) : m_instance(instance) {}

MainWindow::~MainWindow() {
    m_worker.request_stop();
    // jthread joins here; the window is gone, so anything the worker still posts is freed by PostOwned.
}

bool MainWindow::Create(int showCommand) {
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = Proc;
    wc.hInstance = m_instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));  // transparent on Mica
    wc.lpszClassName = kClassName;
    // Each size drawn from the best image in icon.ico, not a scaled 32 px one.
    LoadIconWithScaleDown(m_instance, MAKEINTRESOURCEW(IDI_APP), GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
                          m_icon.put());
    LoadIconWithScaleDown(m_instance, MAKEINTRESOURCEW(IDI_APP), GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                          m_smallIcon.put());
    wc.hIcon = m_icon ? m_icon.get() : LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = m_smallIcon.get();
    if (!RegisterClassExW(&wc)) return false;
    if (!Graphics::Get().Ready()) return false;

    m_taskbarCreated = RegisterWindowMessageW(L"TaskbarButtonCreated");
    m_hwnd = CreateWindowExW(0, kClassName, Config::kAppTitle, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                             600, 600, nullptr, nullptr, m_instance, this);
    if (!m_hwnd) return false;
    RestorePlacement(showCommand);
    PostMessageW(m_hwnd, WM_APP_REFRESH, 0, 0);  // read volumes and NVRAM after the window is visible
    return true;
}

int MainWindow::Run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // Alt+letter opens the client-area menu bar (no native menu to do it).
        if (msg.message == WM_SYSCHAR && GetAncestor(msg.hwnd, GA_ROOT) == m_hwnd &&
            m_menu.OpenByMnemonic(m_hwnd, static_cast<wchar_t>(msg.wParam)))
            continue;
        if (m_accel && TranslateAcceleratorW(m_hwnd, m_accel.get(), &msg)) continue;
        if (IsDialogMessageW(m_hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK MainWindow::Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        self->m_hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (message == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self ? self->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

void MainWindow::OnCreate() {
    ApplyBackdrop(m_hwnd);
    m_fonts.Create(GetDpiForWindow(m_hwnd));
    BuildMenus();
    BuildAccelerators();

    // Creation order is the Tab order: deploy card, list, its buttons, log.
    for (const ButtonSpec& b : kButtons) {
        if (b.id == CmdUp) m_list.Create(m_hwnd, kIdList);
        CreateButton(m_hwnd, static_cast<int>(b.id), b.text ? Text::Get(b.text).c_str() : L"");
        m_looks[b.id] = b.look;
    }
    m_log.Create(m_hwnd, kIdLog);
    m_logVisible = Settings::ReadDword(L"LogVisible", 0) != 0;
    m_showApps = Settings::ReadDword(L"ShowFirmwareApps", 0) != 0;
    ShowWindow(m_log.hwnd(), m_logVisible ? SW_SHOW : SW_HIDE);
    SetWindowTextW(GetDlgItem(m_hwnd, CmdSourceMenu), SourceSummary().c_str());
    m_list.SetPlaceholder(Text::Get(IDS_LIST_READING));
    SetStatus(Level::Info, Text::Get(IDS_STATUS_READING));
    UpdateControls();
}

LRESULT MainWindow::Handle(UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == m_taskbarCreated && m_taskbarCreated) {
        if (!m_taskbar) CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(m_taskbar.GetAddressOf()));
        if (m_taskbar && FAILED(m_taskbar->HrInit())) m_taskbar.Reset();
        return 0;
    }
    switch (message) {
    case WM_CREATE:
        OnCreate();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        Paint();
        return 0;
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) Layout();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        RECT r{0, 0, m_fonts.S(480), MinClientHeight(m_logVisible) - m_fonts.S(m_logVisible ? 80 : 60)};
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, m_fonts.dpi());
        info->ptMinTrackSize = {r.right - r.left, r.bottom - r.top};
        return 0;
    }
    case WM_DPICHANGED: {
        m_fonts.Create(HIWORD(wParam));
        m_menu.Remeasure();
        m_list.Relayout();
        m_log.Relayout();
        const RECT* r = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(m_hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        RedrawWindow(m_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        return 0;
    }
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_DWMCOMPOSITIONCHANGED:
        ApplyBackdrop(m_hwnd);
        RedrawWindow(m_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        return 0;
    case WM_MOUSEMOVE: {
        if (!m_trackingMouse) {
            TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, m_hwnd, 0};
            m_trackingMouse = TrackMouseEvent(&tme) != FALSE;
        }
        if (m_menu.SetHot(m_menu.HitTest({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}))) InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSELEAVE:
        m_trackingMouse = false;
        if (m_menu.SetHot(-1)) InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        const int index = m_menu.HitTest({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        if (index >= 0) m_menu.Open(m_hwnd, index, false);
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_KEYMENU && lParam == 0) {  // Alt alone or F10
            m_menu.Open(m_hwnd, 0, true);
            return 0;
        }
        if ((wParam & 0xFFF0) == SC_KEYMENU && lParam != L' ' && m_menu.OpenByMnemonic(m_hwnd, static_cast<wchar_t>(lParam))) return 0;
        break;
    case WM_MENUSELECT:
        m_menu.OnMenuSelect(wParam, lParam);
        return 0;
    case WM_INITMENUPOPUP:  // every popup (menu bar, source combo, context menus) is updated as it opens
        ApplyMenuState(reinterpret_cast<HMENU>(wParam));
        return 0;
    case WM_MEASUREITEM: {
        auto& mis = *reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (mis.CtlType == ODT_MENU) m_menu.Measure(mis);
        return TRUE;
    }
    case WM_DRAWITEM: {
        const auto& dis = *reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (dis.CtlType == ODT_MENU) m_menu.Draw(dis);
        else if (auto it = m_looks.find(dis.CtlID); it != m_looks.end()) DrawButton(dis, m_fonts, it->second);
        return TRUE;
    }
    case WM_CONTEXTMENU: {
        HWND target = reinterpret_cast<HWND>(wParam);
        POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (p.x == -1 && p.y == -1) {  // keyboard: below the focused control
            RECT r{};
            GetWindowRect(target, &r);
            p = {r.left + m_fonts.S(24), r.top + m_fonts.S(24)};
        }
        ShowContextMenu(target, p);
        return 0;
    }
    case WM_COMMAND:
        OnCommand(LOWORD(wParam), HIWORD(wParam), reinterpret_cast<HWND>(lParam));
        return 0;
    case WM_TIMER:
        if (wParam == kAnimationTimer) {
            const float target = m_progress.permille >= 0 ? static_cast<float>(m_progress.permille) / 1000.0f : m_shown;
            m_shown += (target - m_shown) * 0.18f;
            if (std::abs(target - m_shown) < 0.001f) m_shown = target;
            const bool indeterminate = m_busy && m_progress.permille < 0;
            if (!indeterminate && m_shown == target) KillTimer(m_hwnd, kAnimationTimer);
            const RECT r{static_cast<LONG>(m_progressRect.left) - 1, static_cast<LONG>(m_progressRect.top) - 1,
                         static_cast<LONG>(m_progressRect.right) + 1, static_cast<LONG>(m_progressRect.bottom) + 1};
            InvalidateRect(m_hwnd, &r, FALSE);
        }
        return 0;
    case WM_APP_REFRESH: {
        m_uefi = Efi::IsUefi();
        m_nvram = m_uefi && Core::PrepareFirmwareAccess().has_value();
        if (m_nvram) m_secureBoot = Efi::SecureBootEnabled();
        RefreshTargets();
        RefreshBoot();
        if (const Boot::Row* linux = m_snapshot.Linux()) m_list.Select(linux->entry.number);
        Log(Level::Info, Text::Format(IDS_LOG_STARTED, Config::kAppTitle, Config::kAppVersion,
                                      Text::Get(m_uefi ? IDS_UEFI : IDS_LEGACY_BIOS),
                                      Text::Get(m_secureBoot ? (*m_secureBoot ? IDS_ON : IDS_OFF) : IDS_UNKNOWN)));
        Log(Level::Info, Text::Format(IDS_LOG_NVRAM, m_snapshot.ordered, m_snapshot.rows.size()));
        SetStatus(Level::Info, Text::Get(m_esp && m_loaderPresent ? IDS_STATUS_READY : IDS_STATUS_READY_NO_ESP));
        UpdateControls();
        return 0;
    }
    case WM_APP_LOG: {
        auto msg = TakeOwned<LogMessage>(lParam);
        if (msg->level == Level::Warning || msg->level == Level::Error) SetStatus(msg->level, msg->text);
        Log(msg->level, std::move(msg->text));
        return 0;
    }
    case WM_APP_PROGRESS: {
        auto p = TakeOwned<Core::Progress>(lParam);
        m_progress = *p;
        if (p->permille < 0) {
            SetTaskbar(TBPF_INDETERMINATE);
        } else {
            SetTaskbar(TBPF_NORMAL, p->permille);
        }
        SetStatus(Level::Info, ProgressText(*p));
        StartAnimation();
        return 0;
    }
    case WM_APP_DONE: {
        auto done = TakeOwned<DoneMessage>(lParam);
        if (m_worker.joinable()) m_worker.join();  // the worker has finished its job by now
        OnDeployDone(done->ok, done->canceled, done->error);
        return 0;
    }
    case WM_APP_CONFIRM: {
        if (m_closing) return 0;
        const auto& question = *reinterpret_cast<const std::wstring*>(lParam);
        if (wParam == ConfirmBootPartition)
            return Confirm(m_hwnd, Text::Get(IDS_BOOTPART_HEADING), question, Text::Get(IDS_BOOTPART_OK), true) ? 1 : 0;
        return Confirm(m_hwnd, Text::Get(IDS_BITLOCKER_HEADING), question, Text::Get(IDS_BITLOCKER_OK), true) ? 1 : 0;
    }
    case WM_CLOSE:
        if (m_busy && !Confirm(m_hwnd, Text::Get(IDS_CLOSE_HEADING), Text::Get(IDS_CLOSE_MESSAGE), Text::Get(IDS_CLOSE_OK)))
            return 0;
        m_closing = true;
        m_worker.request_stop();
        SavePlacement();
        DestroyWindow(m_hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(m_hwnd, kAnimationTimer);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(m_hwnd, message, wParam, lParam);
}

}  // namespace DL::Ui
