#include "install/EspInstaller.h"
#include <cstdio>
#include <filesystem>

int main() {
    std::filesystem::path tar = L"C:\\Projekty\\linux-live\\esp.tar";
    std::filesystem::path dest = L"C:\\Projekty\\linux-live\\obj\\test_extract";
    std::error_code ec;
    std::filesystem::remove_all(dest, ec);
    std::filesystem::create_directories(dest, ec);

    auto res = DL::Install::DeployEsp(
        tar,
        L"C:\\",
        dest,
        [](std::uint64_t done, std::uint64_t total) {
            std::printf("Progress: %llu / %llu\n", done, total);
        },
        [](std::wstring msg) {
            std::wprintf(L"LOG: %s\n", msg.c_str());
        },
        std::stop_token{});

    if (!res) {
        std::wprintf(L"FAILED: %s\n", res.error().c_str());
        return 1;
    }

    std::wprintf(L"SUCCESS! Files: %u, Bytes: %llu, Skipped: %u, Loader: %s\n",
                 res->files, res->bytes, res->skipped, res->loaderPath.c_str());

    // Test second run (should skip all since unchanged)
    auto res2 = DL::Install::DeployEsp(
        tar,
        L"C:\\",
        dest,
        [](std::uint64_t, std::uint64_t) {},
        [](std::wstring msg) {
            std::wprintf(L"RUN2 LOG: %s\n", msg.c_str());
        },
        std::stop_token{});

    if (!res2) {
        std::wprintf(L"RUN2 FAILED: %s\n", res2.error().c_str());
        return 2;
    }
    std::wprintf(L"RUN2 SUCCESS! Files: %u, Skipped: %u\n", res2->files, res2->skipped);

    std::filesystem::remove_all(dest, ec);
    return 0;
}
