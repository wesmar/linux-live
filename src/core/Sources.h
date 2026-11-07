// Where the image and the ESP package come from: the GitHub release (default) or custom
// locations - an https:// URL or a local file each. Anyone with their own shim-signed build can
// point the tool at it; the choice is stored per user and shared by the GUI and the command line.
#pragma once
#include "support/Common.h"
#include <string>
#include <string_view>

namespace DL::Sources {

enum class Mode { GitHub, Custom };

// The two parts of a source; their (translated) names are used in messages.
enum class Part { Image, Package };
[[nodiscard]] std::wstring PartName(Part part);  // "image" / "ESP package", lower case

struct Pair {
    std::wstring image;  // filesystem.squashfs
    std::wstring esp;    // esp.tar
};

Pair GitHub();
std::wstring GitHubReleasesPage();

Mode LoadMode();
void SaveMode(Mode mode);
Pair LoadCustom();
void SaveCustom(const Pair& pair);
Pair Active();  // according to the saved mode

bool IsUrl(std::wstring_view source);       // http(s)://...
std::wstring Short(std::wstring_view source);  // "kvc.pl/esp.tar", "D:\build\esp.tar"
[[nodiscard]] Result<> Check(std::wstring_view source, Part part);  // https URL or an existing file

}  // namespace DL::Sources
