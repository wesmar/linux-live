// Per-user settings in HKCU\Software\KVC\LinuxLive (shared by the GUI and the command line).
#pragma once
#include <Windows.h>
#include <string>
#include <string_view>

namespace DL::Settings {

std::wstring Read(const wchar_t* name, std::wstring_view fallback = {});
void Write(const wchar_t* name, const std::wstring& value);
DWORD ReadDword(const wchar_t* name, DWORD fallback);
void WriteDword(const wchar_t* name, DWORD value);
bool Exists(const wchar_t* name);

}  // namespace DL::Settings
