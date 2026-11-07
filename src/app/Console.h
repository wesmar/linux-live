// Console output for the command-line mode of a GUI-subsystem executable.
//
//  * stdout / stderr redirected to a file or a pipe (`> log.txt`, `| findstr`, ssh without a
//    terminal): written there as UTF-8, nothing is attached;
//  * otherwise the console of the parent process is attached (cmd, PowerShell, Windows Terminal,
//    an ssh session with a terminal) and written with WriteConsoleW;
//  * started without any console (shortcut with arguments): a console is allocated and kept
//    open until a key is pressed.
//
// The shell does not wait for a GUI-subsystem program, so when the parent console was attached a
// synthetic Enter is queued at the end: the prompt is redrawn below the output.
#pragma once
#include "support/Common.h"
#include "support/Handle.h"
#include <cstdint>
#include <string>
#include <string_view>

namespace DL::Cli {

class Console {
public:
    Console();
    ~Console();
    Console(const Console&) = delete;
    Console& operator=(const Console&) = delete;

    void Out(std::wstring_view text);  // stdout
    void Err(std::wstring_view text);  // stderr
    void Line(std::wstring_view text = {}) { Out(text); Out(L"\n"); }

    // "[ OK ] text" style status lines on stdout (coloured on a VT console).
    void Status(Level level, std::wstring_view text);

    // Single-line progress bar on stderr, redrawn in place; Status() keeps it below new lines.
    void Progress(std::wstring_view label, double fraction, std::wstring_view detail);
    void EndProgress();  // keeps the last bar, moves to a new line
    void set_progress_enabled(bool enabled) { m_progressEnabled = enabled; }

    [[nodiscard]] bool color() const noexcept { return m_vt; }

private:
    struct Stream {
        HANDLE handle = nullptr;  // not owned (std handle or m_conout)
        bool console = false;
    };
    void Write(Stream& s, std::wstring_view text);
    void ClearProgressLine();
    int Width() const;

    Stream m_out, m_err, m_bar;  // m_bar: where progress bars go
    FileHandle m_conout;       // CONOUT$ when a console is attached / allocated
    bool m_attached = false;   // parent console -> synthetic Enter at exit
    bool m_allocated = false;  // own console -> wait for a key at exit
    bool m_vt = false;
    bool m_progressEnabled = true;
    bool m_progressShown = false;
    bool m_progressComplete = false;  // the 100 % line of a step is drawn once
    std::uint64_t m_lastDraw = 0;
    std::wstring m_lastLabel;
};

}  // namespace DL::Cli
