// Main window: compact, Per-Monitor-V2 aware, Mica backdrop, every action also in the menus.
#pragma once
#include "core/BootList.h"
#include "core/Deploy.h"
#include "gui/Menus.h"
#include "gui/Theme.h"
#include "gui/Widgets.h"
#include "storage/BootPartition.h"
#include "storage/Volumes.h"
#include "support/Text.h"
#include <ShObjIdl.h>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace DL::Ui {

class MainWindow {
public:
    explicit MainWindow(HINSTANCE instance);
    ~MainWindow();
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    bool Create(int showCommand);
    int Run();  // message loop

private:
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT, WPARAM, LPARAM);

    // Construction / layout / painting
    void OnCreate();
    void BuildMenus();
    void BuildAccelerators();
    void Layout();
    void Paint();
    void PaintDeployCard(Canvas& c);
    void PaintBootCard(Canvas& c);
    [[nodiscard]] int MinClientHeight(bool withLog) const;
    [[nodiscard]] float ButtonWidth() const;  // one width for every button, from the longest label
    void SetLogVisible(bool visible, bool resizeWindow);
    void RestorePlacement(int showCommand);
    void SavePlacement();

    // State
    void RefreshTargets();
    void RefreshBoot();
    void UpdateControls();
    void ApplyMenuState(HMENU menu) const;
    [[nodiscard]] bool Can(UINT command) const;
    void SetStatus(Level level, std::wstring text);
    void Log(Level level, std::wstring text);
    void SetTaskbar(TBPFLAG state, int permille = 0);
    void StartAnimation();

    // Commands
    void OnCommand(UINT id, UINT code, HWND control);
    void StartDeploy();
    void OnDeployDone(bool ok, bool canceled, const std::wstring& error);
    void EditCustomSources();
    void SetSourceMode(bool custom);
    void BootAction(UINT command);
    void RestartNow();
    void ShowCommandLineHelp();
    void ShowAbout();
    void OpenFolder(bool esp);
    void RemoveBootPartition();
    void ShowContextMenu(HWND target, POINT screen);
    void SetLanguage(UINT command);
    void ApplyLanguage();  // rebuilds every text after a language switch
    [[nodiscard]] std::wstring LanguageName(const std::wstring& code) const;
    [[nodiscard]] std::wstring SourceSummary() const;

    HINSTANCE m_instance;
    HWND m_hwnd = nullptr;
    Fonts m_fonts;
    MenuBar m_menu{m_fonts};
    HMENU m_sourceMenu = nullptr, m_listMenu = nullptr, m_logMenu = nullptr;
    std::vector<Text::Language> m_languages;  // View menu order
    AccelHandle m_accel;
    IconHandle m_icon, m_smallIcon;  // window class icons, alive as long as the window
    EntryList m_list{m_fonts};
    LogView m_log{m_fonts};
    std::unordered_map<UINT, ButtonLook> m_looks;
    ComPtr<ITaskbarList3> m_taskbar;
    UINT m_taskbarCreated = 0;

    // Layout (client pixels)
    D2D1_RECT_F m_deployCard{}, m_bootCard{}, m_logCard{};
    D2D1_RECT_F m_rowSource{}, m_rowData{}, m_rowEsp{}, m_progressRect{}, m_statusRect{}, m_bootHeader{};
    float m_labelWidth = 0;
    bool m_trackingMouse = false;

    // Environment
    bool m_uefi = false, m_nvram = false;
    std::optional<bool> m_secureBoot;
    std::optional<Storage::DataTarget> m_data;
    std::optional<Storage::EspTarget> m_esp;
    std::optional<Storage::BootPartition> m_boot;  // only for an ESP too small for the kernel
    bool m_kernelOnBoot = false;                    // kernel and initrd are loaded from m_boot
    std::wstring m_dataText, m_dataDetail, m_espText, m_espDetail;
    bool m_loaderPresent = false;
    Boot::Snapshot m_snapshot;

    // View state
    bool m_logVisible = false, m_showApps = false, m_modal = false, m_closing = false;
    std::size_t m_unread = 0;
    Level m_statusLevel = Level::Info;
    std::wstring m_status;

    // Deployment
    bool m_busy = false;
    Core::Progress m_progress;
    float m_shown = 0;  // animated bar position 0..1
    bool m_failed = false, m_finished = false;
    ULONGLONG m_animStart = 0;
    std::jthread m_worker;
};

}  // namespace DL::Ui
