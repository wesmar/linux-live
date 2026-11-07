// User-visible text. English is built in (STRINGTABLE, src/resources/lang/en.rc) and always works;
// every other language is a separate file next to the executable (like IrfanView's language files),
// in the format of the SFTP plugin:
//
//   language\<file>.lng    UTF-8, one "ID=text" per line, '#' comments, escapes \n \r \t \\
//     #name=Polski         the name shown in the menu (in that language)
//     #locale=pl           Windows languages the file covers, comma separated (pl, pt-BR, sr-Latn ...)
//
// Automatic choice: the user's preferred Windows display languages, in their order; for each one a
// file that declares it in #locale, else one named after it (pt-br.lng, pt.lng, por.lng - ISO 639-2 -
// or ptb.lng - the Windows abbreviation). Nothing matches: English. The user can also pick a language
// by hand. A missing or broken string falls back to English one by one, so a partial file still works.
//
// The command line is always English (scripts parse it). `lng-export` writes the English template,
// `lng-check` reports coverage and broken placeholders of the language files.
#pragma once
#include "resources/resource.h"
#include <Windows.h>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace DL::Text {

struct Language {
    std::wstring code;                  // "en" or the file name without .lng ("pl", "pt-br", "deu" ...)
    std::wstring name;                  // "English", "Polski" ...
    std::vector<std::wstring> locales;  // declared by #locale (lower case)
    std::size_t translated = 0;         // strings of the English table present in the file
    std::size_t total = 0;              // strings in the English table
    [[nodiscard]] int Coverage() const { return total ? static_cast<int>(translated * 100 / total) : 100; }
};

// GUI: the saved choice (setting "Language"); "auto" or nothing saved = the Windows language.
void UseSaved();
// Switches to `code` ("en", a file code such as "pl", or "auto") and optionally remembers it.
// False (and English) when the language is not available.
bool Use(std::wstring_view code, bool save = false);
[[nodiscard]] std::wstring Detect();   // the automatic choice, see above
[[nodiscard]] std::wstring Current();  // effective code
[[nodiscard]] bool Automatic();        // following the Windows language
[[nodiscard]] std::vector<Language> Available();  // English first, then the files sorted by name
// Language for system error messages (FormatMessage); English when the UI runs in English.
[[nodiscard]] LANGID SystemLanguage();

[[nodiscard]] std::wstring Get(UINT id);
[[nodiscard]] std::wstring English(UINT id);

// Writes every English string as "ID=text" (the starting point of a new .lng file).
bool ExportTemplate(const std::wstring& path);

// Translation report of one .lng file against the English table.
struct Report {
    Language language;
    std::wstring file;
    std::vector<UINT> missing;       // in English, not in the file
    std::vector<UINT> unknown;       // in the file, not in English (old or mistyped ids)
    std::vector<UINT> placeholders;  // {...} fields differ from English - would fall back to English
};
[[nodiscard]] std::vector<Report> Check(const std::wstring& file = {});  // one file, or every file in language\

namespace detail {
std::wstring VFormat(UINT id, std::wformat_args args);
}

// std::format with a translated pattern. A translation with a broken pattern falls back to English.
template <class... Args>
[[nodiscard]] std::wstring Format(UINT id, const Args&... args) {
    return detail::VFormat(id, std::make_wformat_args(args...));
}

}  // namespace DL::Text
