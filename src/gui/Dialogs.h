// Modal dialogs in the same Mica style as the main window (instead of light MessageBox windows).
#pragma once
#include <Windows.h>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace DL::Ui {

struct DialogField {
    std::wstring label;
    std::wstring value;
    std::wstring cue;           // placeholder text
    const wchar_t* filter = nullptr;  // "Name\0*.ext\0..." for the Browse button; nullptr = no button
};

struct DialogSpec {
    std::wstring heading;
    std::wstring message;       // wrapped paragraph, may be empty
    std::wstring monospace;     // scrollable read-only text block (help), may be empty
    std::vector<DialogField> fields;
    std::wstring ok;                  // empty = "OK" in the current language
    std::wstring cancel;              // empty = "Cancel" in the current language
    bool cancelButton = true;         // false: only OK (and the extra button)
    std::wstring extra;               // optional third button (result 2)
    bool danger = false;              // heading in the critical colour
    int width = 460;                  // DIP
    // Returns an error to show (the dialog stays open) or nullopt to accept the values.
    std::function<std::optional<std::wstring>(const std::vector<std::wstring>&)> validate;
};

struct DialogResult {
    int button = 0;  // 0 = cancel / closed, 1 = ok, 2 = extra
    std::vector<std::wstring> values;
};

DialogResult RunDialog(HWND owner, const DialogSpec& spec);

bool Confirm(HWND owner, std::wstring heading, std::wstring message, std::wstring ok, bool danger = false);
void Inform(HWND owner, std::wstring heading, std::wstring message);

}  // namespace DL::Ui
