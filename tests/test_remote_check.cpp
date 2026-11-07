#include "net/Downloader.h"
#include <cstdio>
#include <filesystem>

int main() {
    auto r1 = DL::Net::CheckRemote(L"https://kvc.pl/filesystem.squashfs");
    if (!r1) {
        std::wprintf(L"CheckRemote image failed: %s\n", r1.error().c_str());
        return 1;
    }
    std::wprintf(L"Remote image size: %llu bytes\n", r1->size);

    auto r2 = DL::Net::CheckRemote(L"https://kvc.pl/esp.tar");
    if (!r2) {
        std::wprintf(L"CheckRemote esp failed: %s\n", r2.error().c_str());
        return 2;
    }
    std::wprintf(L"Remote esp size: %llu bytes\n", r2->size);

    std::filesystem::path img = L"C:\\linux-live\\filesystem.squashfs";
    std::filesystem::path tar = L"C:\\linux-live\\esp.tar";
    std::error_code ec;

    std::wprintf(L"Local image size: %llu (matches: %d)\n",
                 std::filesystem::file_size(img, ec),
                 std::filesystem::file_size(img, ec) == r1->size);

    std::wprintf(L"Local tar size:   %llu (matches: %d)\n",
                 std::filesystem::file_size(tar, ec),
                 std::filesystem::file_size(tar, ec) == r2->size);

    return 0;
}
