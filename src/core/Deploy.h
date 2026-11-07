// Download (or copy) the image and the ESP package, then install the ESP files.
// Shared by the GUI and the command line; everything is reported through Hooks.
#pragma once
#include "core/Sources.h"
#include "storage/Volumes.h"
#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>

namespace DL::Core {

struct Progress {
    int permille = -1;          // overall 0..1000, -1 = indeterminate (checking the server)
    int step = 0, steps = 0;    // 1-based step number / step count (0 = not started)
    std::wstring phase;         // "Downloading image", "Verifying local image", "Writing ESP" ...
    std::uint64_t done = 0;     // bytes of the current step
    std::uint64_t total = 0;    // bytes of the current step (0 = unknown)
    std::uint64_t speed = 0;    // bytes per second
    std::int64_t eta = -1;      // seconds left in the current step, -1 = unknown
};

struct Hooks {
    std::function<void(Level, const std::wstring&)> log;
    std::function<void(const Progress&)> progress;
    ConfirmFn confirm;               // "store the BitLocker recovery password on the ESP?"
    ConfirmFn confirmBootPartition;  // "shrink the Windows volume for a boot partition?" (small ESP)
};

// Deploys the package into the layout it states (ESP folder of its signed GRUB, media folder from
// grub.cfg) and returns that layout. `esp` is the chosen ESP; its deployed layout, if any, tells
// where the local copies of the previous package are.
Result<Install::Layout> Deploy(const Sources::Pair& source, const Storage::EspTarget& esp, const Hooks& hooks,
                               std::stop_token stop);

}  // namespace DL::Core
