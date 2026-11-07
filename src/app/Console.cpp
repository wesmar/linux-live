#include "app/Console.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <format>

namespace DL::Cli {
namespace {

enum class Kind { None, Console, Redirected };

Kind KindOf(HANDLE h) {
    if (!h || h == INVALID_HANDLE_VALUE) return Kind::None;
    switch (GetFileType(h)) {
    case FILE_TYPE_DISK:
    case FILE_TYPE_PIPE: return Kind::Redirected;
    case FILE_TYPE_CHAR: return Kind::Console;
    default: return Kind::None;
    }
}

FileHandle OpenConsole(const wchar_t* name) {
    return FileHandle(CreateFileW(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, 0, nullptr));
}

void QueueEnter() {
    // CONIN$ of the attached console (works for conhost, Windows Terminal and ssh's ConPTY).
    const FileHandle in = OpenConsole(L"CONIN$");
    if (!in) return;
    INPUT_RECORD keys[2]{};
    keys[0].EventType = KEY_EVENT;
    keys[0].Event.KeyEvent.bKeyDown = TRUE;
    keys[0].Event.KeyEvent.wRepeatCount = 1;
    keys[0].Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
    keys[0].Event.KeyEvent.wVirtualScanCode = static_cast<WORD>(MapVirtualKeyW(VK_RETURN, MAPVK_VK_TO_VSC));
    keys[0].Event.KeyEvent.uChar.UnicodeChar = L'\r';
    keys[1] = keys[0];
    keys[1].Event.KeyEvent.bKeyDown = FALSE;
    DWORD written = 0;
    WriteConsoleInputW(in.get(), keys, 2, &written);
}

void WaitForKey() {
    const FileHandle in = OpenConsole(L"CONIN$");
    if (!in) return;
    FlushConsoleInputBuffer(in.get());
    for (;;) {
        INPUT_RECORD record{};
        DWORD read = 0;
        if (!ReadConsoleInputW(in.get(), &record, 1, &read) || !read) break;
        if (record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown) break;
    }
}

}  // namespace

Console::Console() {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE), err = GetStdHandle(STD_ERROR_HANDLE);
    const Kind outKind = KindOf(out), errKind = KindOf(err);
    if (outKind == Kind::Redirected) m_out.handle = out;
    if (errKind == Kind::Redirected) m_err.handle = err;

    if (!m_out.handle || !m_err.handle) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) m_attached = true;
        else if (!m_out.handle && !m_err.handle && AllocConsole()) m_allocated = true;
        if (m_attached || m_allocated) {
            m_conout = OpenConsole(L"CONOUT$");
        }
        if (m_conout) {
            if (!m_out.handle) m_out = {m_conout.get(), true};
            if (!m_err.handle) m_err = {m_conout.get(), true};
            DWORD mode = 0;
            if (GetConsoleMode(m_conout.get(), &mode))
                m_vt = SetConsoleMode(m_conout.get(), mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
        }
    }
    if (!m_out.handle) m_out = m_err;
    if (!m_err.handle) m_err = m_out;
    // Progress belongs on a terminal. Without one it goes to stdout with the status lines, so the
    // two cannot be reordered (ssh without a terminal carries stdout and stderr separately).
    m_bar = m_err.console ? m_err : m_out;
    // Colour only when stdout itself is the console: never escape codes in a file or a pipe.
    m_vt = m_vt && m_out.console;
    if (m_allocated) SetConsoleTitleW(L"Linux Live");
}

Console::~Console() {
    EndProgress();
    if (m_allocated) {
        Out(L"\nPress any key to close this window.");
        WaitForKey();
    } else if (m_attached) {
        QueueEnter();
    }
    m_conout.reset();
    if (m_attached || m_allocated) FreeConsole();
}

void Console::Write(Stream& s, std::wstring_view text) {
    if (!s.handle || text.empty()) return;
    DWORD written = 0;
    if (s.console) {
        WriteConsoleW(s.handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        return;
    }
    // Files and pipes get UTF-8 with CRLF line ends (cmd, findstr and Notepad all expect them).
    std::wstring crlf;
    crlf.reserve(text.size() + 8);
    for (wchar_t c : text) {
        if (c == L'\n' && (crlf.empty() || crlf.back() != L'\r')) crlf.push_back(L'\r');
        crlf.push_back(c);
    }
    const std::string utf8 = ToUtf8(crlf);
    WriteFile(s.handle, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

void Console::Out(std::wstring_view text) {
    ClearProgressLine();
    Write(m_out, text);
}

void Console::Err(std::wstring_view text) {
    ClearProgressLine();
    Write(m_err, text);
}

void Console::Status(Level level, std::wstring_view text) {
    static constexpr struct { const wchar_t* tag; const wchar_t* color; } kTags[] = {
        {L"[INFO]", L"\x1b[90m"}, {L"[ OK ]", L"\x1b[32m"}, {L"[WARN]", L"\x1b[33m"}, {L"[FAIL]", L"\x1b[91m"}};
    const auto& t = kTags[static_cast<int>(level)];
    std::wstring line = m_vt ? std::format(L"{}{}\x1b[0m {}\n", t.color, t.tag, text) : std::format(L"{} {}\n", t.tag, text);
    if (level == Level::Error) Err(line);
    else Out(line);
}

int Console::Width() const {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (m_bar.console && GetConsoleScreenBufferInfo(m_bar.handle, &info))
        return std::clamp<int>(info.srWindow.Right - info.srWindow.Left + 1, 40, 200);
    return 80;
}

void Console::ClearProgressLine() {
    if (!m_progressShown) return;
    m_progressShown = false;
    if (m_bar.console) Write(m_bar, std::wstring(L"\r") + std::wstring(static_cast<std::size_t>(Width() - 1), L' ') + L"\r");
    else Write(m_bar, L"\n");
}

void Console::Progress(std::wstring_view label, double fraction, std::wstring_view detail) {
    if (!m_progressEnabled) return;
    if (m_lastLabel != label) {
        EndProgress();
        m_lastLabel = label;
        m_progressComplete = false;
    }
    // A terminal gets ~10 redraws per second, a pipe or file one line per second at most.
    const std::uint64_t now = GetTickCount64();
    const bool complete = fraction >= 1.0;
    if (complete && m_progressComplete) return;
    if (!complete && now - m_lastDraw < (m_bar.console ? 100u : 1000u)) return;
    m_lastDraw = now;
    m_progressComplete = complete;

    // Fixed columns, so consecutive lines (a pipe keeps them all) line up:
    //   "  [2/3] Downloading image           ██████░░░░  42%  480.2 MB / 1.07 GB  54.9 MB/s  ETA 0:12"
    fraction = std::clamp(fraction, 0.0, 1.0);
    const int width = m_bar.console ? Width() : 120;
    constexpr std::size_t kLabel = 30;
    constexpr int kTail = 48;  // room reserved for " 100%  1.07 GB / 1.07 GB  54.9 MB/s  ETA 1:02:03"
    const std::wstring fitted = label.size() > kLabel ? std::wstring(label.substr(0, kLabel - 1)) + L"\x2026" : std::wstring(label);
    const std::wstring head = std::format(L"  {:<{}} ", fitted, kLabel);
    const std::wstring tail = std::format(L" {:>3}%  {}", static_cast<int>(std::floor(fraction * 100.0)), detail);
    const int barWidth = std::clamp(width - static_cast<int>(head.size()) - kTail, 10, 40);
    const int filled = static_cast<int>(std::round(fraction * barWidth));
    std::wstring bar;
    bar.reserve(static_cast<std::size_t>(barWidth));
    for (int i = 0; i < barWidth; ++i) bar.push_back(i < filled ? L'\x2588' : L'\x2591');  // █ ░
    std::wstring line = head + (m_vt ? L"\x1b[38;2;215;10;83m" + bar + L"\x1b[0m" : bar) + tail;
    if (static_cast<int>(head.size() + bar.size() + tail.size()) >= width)
        line.resize(line.size() - (head.size() + bar.size() + tail.size() - static_cast<std::size_t>(width) + 1));

    DWORD written = 0;
    const std::wstring text = (m_bar.console ? L"\r" : L"") + line + (m_bar.console ? L"" : L"\n");
    if (m_bar.console) {
        WriteConsoleW(m_bar.handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        m_progressShown = true;
    } else {
        Write(m_bar, text);
    }
}

void Console::EndProgress() {
    if (m_progressShown && m_bar.console) {
        Write(m_bar, L"\n");
        m_progressShown = false;
    }
    m_lastLabel.clear();
    m_lastDraw = 0;
}

}  // namespace DL::Cli
