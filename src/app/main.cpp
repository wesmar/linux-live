// Linux Live - entry point. No arguments: graphical interface. Any argument: command line.
#include "app/Cli.h"
#include "gui/MainWindow.h"
#include "support/Common.h"
#include "support/Config.h"
#include "support/Text.h"
#include <Windows.h>
#include <CommCtrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <memory>
#include <string>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

namespace {

struct ArgvDeleter {
    void operator()(wchar_t** argv) const noexcept { LocalFree(argv); }
};

class ComApartment {
public:
    ComApartment() noexcept : m_hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
    ~ComApartment() { if (SUCCEEDED(m_hr)) CoUninitialize(); }

private:
    HRESULT m_hr;
};

std::wstring ModulePath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n == 0) return {};
        if (n < path.size()) {
            path.resize(n);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

// The GUI needs administrator rights for NVRAM and the ESP; the manifest stays asInvoker so the
// command line can report a missing elevation instead of opening a UAC prompt in a script or ssh.
int RelaunchElevated() {
    const std::wstring self = ModulePath();
    SHELLEXECUTEINFOW info{sizeof info};
    info.fMask = SEE_MASK_NOASYNC;
    info.lpVerb = L"runas";
    info.lpFile = self.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&info)) return 0;
    if (GetLastError() != ERROR_CANCELLED)
        MessageBoxW(nullptr, DL::ErrorText(DL::Text::Get(IDS_ERR_ELEVATE), GetLastError()).c_str(), DL::Config::kAppTitle,
                    MB_OK | MB_ICONERROR);
    return 1;
}

}  // namespace

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int showCommand) {
    // Hardening: DLLs only from System32 (no planting next to the executable or in the working directory).
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);

    int argc = 0;
    const std::unique_ptr<wchar_t*, ArgvDeleter> argv(CommandLineToArgvW(GetCommandLineW(), &argc));
    if (argv && argc > 1) return DL::Cli::Run(argc, argv.get());

    DL::Text::UseSaved();  // Windows language unless the user picked one
    if (!DL::IsElevated()) return RelaunchElevated();

    const ComApartment com;  // file dialogs, taskbar progress
    const INITCOMMONCONTROLSEX controls{sizeof controls, ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);

    DL::Ui::MainWindow window(instance);
    if (!window.Create(showCommand)) {
        MessageBoxW(nullptr, DL::Text::Get(IDS_ERR_WINDOW).c_str(), DL::Config::kAppTitle, MB_OK | MB_ICONERROR);
        return 1;
    }
    return window.Run();
}
