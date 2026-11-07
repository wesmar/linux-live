#include "support/Text.h"
#include "support/Settings.h"
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

namespace DL::Text {
namespace {
namespace fs = std::filesystem;

constexpr LANGID kEnglish = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);

struct State {
    std::shared_mutex lock;  // the worker thread reads strings while the UI may switch language
    std::wstring code = L"en";
    bool automatic = false;
    std::unordered_map<UINT, std::wstring> strings;  // the .lng file; empty = English
};

State& S() {
    static State state;
    return state;
}

std::wstring Lower(std::wstring_view s) {
    std::wstring out(s);
    for (auto& c : out) c = static_cast<wchar_t>(std::towlower(c));
    return out;
}

HMODULE Module() {
    static const HMODULE module = [] {
        HMODULE m = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&Module), &m);
        return m;
    }();
    return module;
}

// String tables are stored in blocks of 16 length-prefixed strings.
std::wstring LoadResourceString(UINT id, LANGID language) {
    HMODULE module = Module();
    HRSRC info = FindResourceExW(module, RT_STRING, MAKEINTRESOURCEW(id / 16 + 1), language);
    if (!info) return {};
    HGLOBAL data = LoadResource(module, info);
    const auto* p = static_cast<const WCHAR*>(data ? LockResource(data) : nullptr);
    if (!p) return {};
    const WCHAR* end = p + SizeofResource(module, info) / sizeof(WCHAR);
    for (UINT i = 0; i < id % 16 && p < end; ++i) p += 1 + *p;
    if (p >= end || p + 1 + *p > end) return {};
    return std::wstring(p + 1, *p);
}

// Every id of the English table, ascending (read once).
const std::vector<UINT>& EnglishIds() {
    static const std::vector<UINT> ids = [] {
        std::vector<UINT> blocks, result;
        EnumResourceNamesW(
            Module(), RT_STRING,
            [](HMODULE, LPCWSTR, LPWSTR name, LONG_PTR param) -> BOOL {
                if (IS_INTRESOURCE(name))
                    reinterpret_cast<std::vector<UINT>*>(param)->push_back(static_cast<UINT>(reinterpret_cast<ULONG_PTR>(name)));
                return TRUE;
            },
            reinterpret_cast<LONG_PTR>(&blocks));
        std::ranges::sort(blocks);
        for (UINT block : blocks)
            for (UINT i = 0; i < 16; ++i)
                if (const UINT id = (block - 1) * 16 + i; !LoadResourceString(id, kEnglish).empty()) result.push_back(id);
        return result;
    }();
    return ids;
}

fs::path LanguageDirectory() {
    std::wstring self(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(Module(), self.data(), static_cast<DWORD>(self.size()));
        if (n == 0) return {};
        if (n < self.size()) {
            self.resize(n);
            break;
        }
        self.resize(self.size() * 2);
    }
    return fs::path(self).parent_path() / L"language";
}

std::wstring Widen(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::string Narrow(std::wstring_view text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Unescape(std::wstring_view raw) {
    std::wstring out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == L'\\' && i + 1 < raw.size()) {
            switch (raw[i + 1]) {
            case L'n': out += L'\n'; ++i; continue;
            case L'r': out += L'\r'; ++i; continue;
            case L't': out += L'\t'; ++i; continue;
            case L'\\': out += L'\\'; ++i; continue;
            default: break;
            }
        }
        out += raw[i];
    }
    return out;
}

std::wstring Escape(std::wstring_view text) {
    std::wstring out;
    for (wchar_t c : text) {
        switch (c) {
        case L'\n': out += L"\\n"; break;
        case L'\r': out += L"\\r"; break;
        case L'\t': out += L"\\t"; break;
        case L'\\': out += L"\\\\"; break;
        default: out += c; break;
        }
    }
    return out;
}

bool ValidCode(std::wstring_view code) {
    return !code.empty() && code.size() <= 16 &&
           std::ranges::all_of(code, [](wchar_t c) { return std::iswalnum(c) || c == L'-' || c == L'_'; });
}

struct LngFile {
    Language language;
    fs::path path;
    std::unordered_map<UINT, std::wstring> strings;
};

// Parses a .lng file: "#name=", "#locale=" headers and (when `strings`) the ID=text lines.
bool ReadLng(const fs::path& path, LngFile& out, bool strings) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.path = path;
    out.language.code = Lower(path.stem().wstring());
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (first && line.starts_with("\xEF\xBB\xBF")) line.erase(0, 3);  // UTF-8 BOM
        first = false;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.starts_with("#name=")) {
            out.language.name = Widen(std::string_view(line).substr(6));
            continue;
        }
        if (line.starts_with("#locale=")) {
            const std::wstring list = Lower(Widen(std::string_view(line).substr(8)));
            for (std::size_t start = 0; start <= list.size();) {
                std::size_t end = list.find(L',', start);
                if (end == std::wstring::npos) end = list.size();
                std::wstring tag = list.substr(start, end - start);
                std::erase_if(tag, [](wchar_t c) { return std::iswspace(c) != 0; });
                std::ranges::replace(tag, L'_', L'-');
                if (!tag.empty()) out.language.locales.push_back(std::move(tag));
                start = end + 1;
            }
            continue;
        }
        if (!strings || line.empty() || line[0] == '#') continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0 || eq > 9) continue;
        UINT id = 0;
        bool numeric = true;
        for (std::size_t i = 0; i < eq; ++i) {
            if (line[i] < '0' || line[i] > '9') { numeric = false; break; }
            id = id * 10 + static_cast<UINT>(line[i] - '0');
        }
        if (numeric) out.strings[id] = Unescape(Widen(std::string_view(line).substr(eq + 1)));
    }
    if (out.language.name.empty()) out.language.name = out.language.code;
    return true;
}

std::vector<LngFile> ScanFiles(bool strings) {
    std::vector<LngFile> files;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(LanguageDirectory(), ec)) {
        if (!entry.is_regular_file(ec) || Lower(entry.path().extension().wstring()) != L".lng") continue;
        const std::wstring code = Lower(entry.path().stem().wstring());
        if (!ValidCode(code) || code == L"en") continue;
        LngFile file;
        if (ReadLng(entry.path(), file, strings)) files.push_back(std::move(file));
    }
    return files;
}

// {...} replacement fields of a std::format pattern.
struct Fields {
    std::size_t automatic = 0;          // "{}" / "{:spec}"
    std::vector<std::size_t> indexed;   // "{n}" / "{n:spec}"
    std::vector<std::wstring> specs;    // ":spec" parts
    bool valid = true;
};

Fields ParseFields(std::wstring_view s) {
    Fields f;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'{') {
            if (i + 1 < s.size() && s[i + 1] == L'{') { ++i; continue; }  // "{{" escape
            const std::size_t close = s.find(L'}', i);
            if (close == std::wstring_view::npos) { f.valid = false; break; }
            const std::wstring_view inside = s.substr(i + 1, close - i - 1);
            const std::size_t colon = inside.find(L':');
            const std::wstring_view id = inside.substr(0, colon);
            if (colon != std::wstring_view::npos) f.specs.emplace_back(inside.substr(colon));
            if (id.empty()) {
                ++f.automatic;
            } else if (std::ranges::all_of(id, [](wchar_t c) { return c >= L'0' && c <= L'9'; })) {
                f.indexed.push_back(static_cast<std::size_t>(std::wcstoul(std::wstring(id).c_str(), nullptr, 10)));
            } else {
                f.valid = false;
            }
            i = close;
        } else if (s[i] == L'}') {
            if (i + 1 < s.size() && s[i + 1] == L'}') { ++i; continue; }  // "}}" escape
            f.valid = false;
        }
    }
    if (f.automatic && !f.indexed.empty()) f.valid = false;  // std::format forbids mixing the two
    return f;
}

// A translation may leave values out (word order, case forms), but may not reference values the
// English pattern does not pass, use another format spec or be malformed.
bool CompatibleFields(std::wstring_view english, std::wstring_view translation) {
    const Fields en = ParseFields(english), tr = ParseFields(translation);
    if (!tr.valid) return false;
    std::size_t args = en.automatic;
    for (std::size_t i : en.indexed) args = std::max(args, i + 1);
    // Numbered patterns always receive a name twice, {0} in lower case and {1} capitalised; English
    // may use only one of them ("the {0}"), a translation the other ("{1}: ...").
    if (!en.indexed.empty()) args = std::max<std::size_t>(args, 2);
    if (tr.automatic > args) return false;
    if (std::ranges::any_of(tr.indexed, [&](std::size_t i) { return i >= args; })) return false;
    return std::ranges::all_of(tr.specs, [&](const std::wstring& s) { return std::ranges::contains(en.specs, s); });
}

}  // namespace

// ---------------------------------------------------------------------------
// Choosing the language
// ---------------------------------------------------------------------------
std::wstring Detect() {
    ULONG count = 0, size = 0;
    if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size) || !size) return L"en";
    std::wstring buffer(size, L'\0');
    if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buffer.data(), &size)) return L"en";

    const std::vector<LngFile> files = ScanFiles(false);
    auto declared = [&](const std::wstring& tag) -> const LngFile* {
        for (const auto& f : files)
            if (std::ranges::contains(f.language.locales, tag)) return &f;
        return nullptr;
    };
    auto named = [&](const std::wstring& stem) -> const LngFile* {
        for (const auto& f : files)
            if (f.language.code == stem) return &f;
        return nullptr;
    };
    auto localeName = [](const wchar_t* tag, LCTYPE type) {
        wchar_t value[16]{};
        return GetLocaleInfoEx(tag, type, value, static_cast<int>(std::size(value))) ? Lower(value) : std::wstring();
    };

    // The user's display languages in their order (Settings > Time & language > Language).
    for (const wchar_t* p = buffer.c_str(); *p; p += wcslen(p) + 1) {
        const std::wstring tag = Lower(p);                           // "pt-br"
        const std::wstring primary = tag.substr(0, tag.find(L'-'));  // "pt"
        if (primary == L"en") return L"en";
        const LngFile* match = declared(tag);
        if (!match) match = named(tag);
        if (!match) match = declared(primary);
        if (!match) match = named(primary);
        if (!match) match = named(localeName(p, LOCALE_SISO639LANGNAME2));  // "por", "pol", "deu"
        if (!match) match = named(localeName(p, LOCALE_SABBREVLANGNAME));   // "ptb", "plk", "deu"
        if (match) return match->language.code;
    }
    return L"en";
}

bool Automatic() {
    std::shared_lock guard(S().lock);
    return S().automatic;
}

bool Use(std::wstring_view requested, bool save) {
    std::wstring code = Lower(requested);
    const bool automatic = code.empty() || code == L"auto";
    if (automatic) code = Detect();
    LngFile file;
    bool ok = true;
    if (code != L"en") {
        // Only a plain file name, never a path (no "..\" tricks).
        ok = ValidCode(code) && ReadLng(LanguageDirectory() / (code + L".lng"), file, true);
        if (!ok) code = L"en";
    }
    {
        std::unique_lock guard(S().lock);
        S().code = code;
        S().automatic = automatic;
        S().strings = std::move(file.strings);
    }
    if (save && ok) Settings::Write(L"Language", automatic ? std::wstring(L"auto") : code);
    return ok;
}

void UseSaved() {
    // A saved language that has gone (its .lng file was removed) falls back to the Windows language.
    if (!Use(Settings::Read(L"Language", L"auto"))) Use(L"auto");
}

std::wstring Current() {
    std::shared_lock guard(S().lock);
    return S().code;
}

std::vector<Language> Available() {
    const auto& ids = EnglishIds();
    std::vector<Language> list;
    for (auto& f : ScanFiles(true)) {
        f.language.total = ids.size();
        f.language.translated = static_cast<std::size_t>(std::ranges::count_if(ids, [&](UINT id) {
            const auto it = f.strings.find(id);
            return it != f.strings.end() && !it->second.empty();
        }));
        list.push_back(std::move(f.language));
    }
    std::ranges::sort(list, [](const Language& a, const Language& b) {
        return CompareStringEx(LOCALE_NAME_INVARIANT, LINGUISTIC_IGNORECASE, a.name.c_str(), -1, b.name.c_str(), -1, nullptr, nullptr, 0) ==
               CSTR_LESS_THAN;
    });
    Language english{L"en", L"English", {L"en"}, ids.size(), ids.size()};
    list.insert(list.begin(), std::move(english));
    return list;
}

LANGID SystemLanguage() {
    std::shared_lock guard(S().lock);
    if (S().code == L"en") return kEnglish;
    const LCID lcid = LocaleNameToLCID(S().code.c_str(), 0);  // "pl" -> Polish, "pt-br" -> Portuguese (Brazil)
    return lcid ? LANGIDFROMLCID(lcid) : 0;                  // 0 = the user's default
}

// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------
std::wstring English(UINT id) {
    std::wstring text = LoadResourceString(id, kEnglish);
    return text.empty() ? std::format(L"#{}", id) : text;
}

std::wstring Get(UINT id) {
    {
        std::shared_lock guard(S().lock);
        if (auto it = S().strings.find(id); it != S().strings.end() && !it->second.empty()) return it->second;
    }
    return English(id);
}

namespace detail {
std::wstring VFormat(UINT id, std::wformat_args args) {
    const std::wstring pattern = Get(id);
    try {
        return std::vformat(pattern, args);
    } catch (const std::format_error&) {
        try {
            return std::vformat(English(id), args);  // the translation has a broken placeholder
        } catch (const std::format_error&) {
            return pattern;
        }
    }
}
}  // namespace detail

// ---------------------------------------------------------------------------
// Tools for translators
// ---------------------------------------------------------------------------
bool ExportTemplate(const std::wstring& path) {
    std::ofstream out(fs::path(path), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << Narrow(L"# Linux Live language file. Translate the text after '=', keep the numbers and the {...} fields:\n"
                  L"# they may move or be left out, never add new ones. In numbered patterns {0} is a name in lower case\n"
                  L"# and {1} the same name capitalised (\"image\" / \"Image\") - use the one your grammar needs.\n"
                  L"# UTF-8. Escapes: \\n \\r \\t \\\\\n"
                  L"# Save as language\\<code>.lng next to LinuxLive.exe, e.g. language\\de.lng, and check it with\n"
                  L"#   LinuxLive.exe lng-check language\\de.lng\n"
                  L"# #name   = the language's own name for the menu, e.g. Deutsch\n"
                  L"# #locale = the Windows display languages this file is for, e.g. de or pt-BR,pt\n"
                  L"#name=English\n#locale=en\n\n");
    for (UINT id : EnglishIds()) out << Narrow(std::format(L"{}={}\n", id, Escape(LoadResourceString(id, kEnglish))));
    return static_cast<bool>(out);
}

std::vector<Report> Check(const std::wstring& path) {
    std::vector<LngFile> files;
    if (path.empty()) {
        files = ScanFiles(true);
    } else if (LngFile f; ReadLng(fs::path(path), f, true)) {
        files.push_back(std::move(f));
    }
    const auto& ids = EnglishIds();
    const std::unordered_set<UINT> known(ids.begin(), ids.end());
    std::vector<Report> reports;
    for (auto& f : files) {
        Report r;
        r.file = f.path.wstring();
        for (UINT id : ids) {
            const auto it = f.strings.find(id);
            if (it == f.strings.end() || it->second.empty()) r.missing.push_back(id);
            else if (!CompatibleFields(LoadResourceString(id, kEnglish), it->second)) r.placeholders.push_back(id);
        }
        for (const auto& [id, text] : f.strings)
            if (!known.contains(id)) r.unknown.push_back(id);
        std::ranges::sort(r.unknown);
        r.language = std::move(f.language);
        r.language.total = ids.size();
        r.language.translated = ids.size() - r.missing.size();
        reports.push_back(std::move(r));
    }
    return reports;
}

}  // namespace DL::Text
