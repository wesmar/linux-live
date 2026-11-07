#include "core/Sources.h"
#include "support/Config.h"
#include "support/Settings.h"
#include "support/Text.h"
#include <Windows.h>
#include <cwctype>
#include <filesystem>
#include <format>

namespace DL::Sources {
namespace {

bool StartsNoCase(std::wstring_view s, std::wstring_view prefix) {
    if (s.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i)
        if (std::towlower(s[i]) != std::towlower(prefix[i])) return false;
    return true;
}

// Earlier builds stored the URLs as ImageUrl / EspUrl; they become the custom source.
void Migrate() {
    if (Settings::Exists(L"CustomImage") || Settings::Exists(L"CustomEsp")) return;
    const std::wstring image = Settings::Read(L"ImageUrl"), esp = Settings::Read(L"EspUrl");
    if (image.empty() && esp.empty()) return;
    Settings::Write(L"CustomImage", image.empty() ? std::wstring(Config::kLegacyImageUrl) : image);
    Settings::Write(L"CustomEsp", esp.empty() ? std::wstring(Config::kLegacyEspUrl) : esp);
}

}  // namespace

Pair GitHub() {
    const std::wstring base = std::format(L"https://github.com/{}/releases/latest/download/", Config::kGitHubRepo);
    return {base + Config::kImageAsset, base + Config::kEspAsset};
}

std::wstring GitHubReleasesPage() { return std::format(L"https://github.com/{}/releases", Config::kGitHubRepo); }

Mode LoadMode() { return Settings::Read(L"SourceMode") == L"custom" ? Mode::Custom : Mode::GitHub; }

void SaveMode(Mode mode) { Settings::Write(L"SourceMode", mode == Mode::Custom ? L"custom" : L"github"); }

Pair LoadCustom() {
    Migrate();
    return {Settings::Read(L"CustomImage"), Settings::Read(L"CustomEsp")};
}

void SaveCustom(const Pair& pair) {
    Settings::Write(L"CustomImage", pair.image);
    Settings::Write(L"CustomEsp", pair.esp);
}

Pair Active() {
    if (LoadMode() == Mode::GitHub) return GitHub();
    Pair custom = LoadCustom();
    const Pair github = GitHub();
    if (custom.image.empty()) custom.image = github.image;
    if (custom.esp.empty()) custom.esp = github.esp;
    return custom;
}

bool IsUrl(std::wstring_view source) { return StartsNoCase(source, L"https://") || StartsNoCase(source, L"http://"); }

std::wstring Short(std::wstring_view source) {
    if (!IsUrl(source)) return std::wstring(source);
    std::wstring_view rest = source.substr(source.find(L"//") + 2);
    const std::size_t slash = rest.find(L'/');
    const std::wstring_view host = rest.substr(0, slash);
    const std::size_t last = source.rfind(L'/');
    const std::wstring_view file = last == std::wstring_view::npos ? std::wstring_view{} : source.substr(last + 1);
    if (StartsNoCase(host, L"github.com")) {
        // github.com/<owner>/<repo>/releases/... -> github.com/<owner>/<repo> · <file>
        std::wstring_view path = slash == std::wstring_view::npos ? std::wstring_view{} : rest.substr(slash + 1);
        const std::size_t a = path.find(L'/');
        const std::size_t b = a == std::wstring_view::npos ? a : path.find(L'/', a + 1);
        if (b != std::wstring_view::npos) return std::format(L"github.com/{} · {}", path.substr(0, b), file);
    }
    return std::format(L"{}/{}", host, file);
}

std::wstring PartName(Part part) { return Text::Get(part == Part::Image ? IDS_PART_IMAGE : IDS_PART_PACKAGE); }

Result<> Check(std::wstring_view source, Part part) {
    const std::wstring name = PartName(part), title = Capitalize(name);
    if (source.empty()) return Fail(Text::Format(IDS_SOURCE_EMPTY, name, title));
    if (StartsNoCase(source, L"http://")) return Fail(Text::Format(IDS_SOURCE_HTTPS, name, title));
    if (StartsNoCase(source, L"https://")) return {};
    std::error_code ec;
    if (!std::filesystem::is_regular_file(std::filesystem::path(source), ec))
        return Fail(Text::Format(IDS_SOURCE_NO_FILE, name, title, source));
    return {};
}

}  // namespace DL::Sources
