#include "core/Deploy.h"
#include "install/EspInstaller.h"
#include "net/Downloader.h"
#include "storage/BitLocker.h"
#include "storage/BootPartition.h"
#include "support/Config.h"
#include "support/Handle.h"
#include "support/Settings.h"
#include "support/Sha256.h"
#include "support/Text.h"
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <string_view>
#include <vector>

namespace DL::Core {
namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Progress over a list of weighted steps. A skipped step gets weight 0, so the
// overall bar can only jump forward, never back.
// ---------------------------------------------------------------------------
class Tracker {
public:
    explicit Tracker(const Hooks& hooks) : m_hooks(hooks) {}

    // A conditional step (a download that only happens if the local copy fails verification)
    // is not counted in "n/m" until it actually starts.
    std::size_t Add(std::wstring name, double weight, bool conditional = false) {
        m_steps.push_back({std::move(name), weight, conditional});
        return m_steps.size() - 1;
    }
    void Skip(std::size_t i) { m_steps[i].weight = 0; }

    void Begin(std::size_t i) {
        m_current = i;
        m_steps[i].conditional = false;
        m_start = Clock::now();
        m_last = m_start - std::chrono::seconds(1);
        Report(0, 0, true);
    }

    // Throttled to ~10 updates per second; the final update of a step is always sent.
    void Update(std::uint64_t done, std::uint64_t total) { Report(done, total, total && done >= total); }

    void Indeterminate(std::wstring phase) const {
        if (m_hooks.progress) m_hooks.progress(Progress{-1, 0, 0, std::move(phase)});
    }

private:
    struct Step {
        std::wstring name;
        double weight;
        bool conditional;
    };

    int Visible(std::size_t i, int* count) const {
        int number = 0, n = 0;
        for (std::size_t k = 0; k < m_steps.size(); ++k) {
            if (m_steps[k].weight <= 0 || m_steps[k].conditional) continue;
            ++n;
            if (k <= i) number = n;
        }
        *count = n;
        return number;
    }

    void Report(std::uint64_t done, std::uint64_t total, bool force) {
        if (!m_hooks.progress) return;
        const auto now = Clock::now();
        if (!force && now - m_last < std::chrono::milliseconds(100)) return;
        m_last = now;

        double all = 0, before = 0;
        for (std::size_t k = 0; k < m_steps.size(); ++k) {
            all += m_steps[k].weight;
            if (k < m_current) before += m_steps[k].weight;
        }
        const double fraction = total ? std::clamp(static_cast<double>(done) / static_cast<double>(total), 0.0, 1.0) : 0.0;
        const double overall = all > 0 ? (before + m_steps[m_current].weight * fraction) / all : 0.0;

        Progress p;
        p.permille = static_cast<int>(overall * 1000.0);
        p.step = Visible(m_current, &p.steps);
        p.phase = m_steps[m_current].name;
        p.done = done;
        p.total = total;
        const double seconds = std::chrono::duration<double>(now - m_start).count();
        if (seconds > 0.25 && done > 0) {
            p.speed = static_cast<std::uint64_t>(static_cast<double>(done) / seconds);
            if (total > done && p.speed) p.eta = static_cast<std::int64_t>((total - done) / p.speed);
            else if (total && done >= total) p.eta = 0;
        }
        m_hooks.progress(p);
    }

    const Hooks& m_hooks;
    std::vector<Step> m_steps;
    std::size_t m_current = 0;
    Clock::time_point m_start{}, m_last{};
};

// ---------------------------------------------------------------------------
// Local files
// ---------------------------------------------------------------------------
std::uint64_t FileTimeStamp(const fs::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &info)) return 0;
    return (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
}

std::uint64_t SizeOf(const fs::path& p) {
    std::error_code ec;
    const auto size = fs::is_regular_file(p, ec) ? fs::file_size(p, ec) : 0;
    return ec ? 0 : size;
}

// SHA-256 of a local file with progress; empty on failure or cancel.
Result<std::wstring> HashFile(const fs::path& path, Tracker& tracker, std::stop_token stop) {
    FileHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file) return Fail(ErrorText(Text::Format(IDS_CTX_OPEN, path.wstring()), GetLastError()));
    Sha256 hash;
    if (!hash.ok()) return Fail(Text::Get(IDS_SHA_UNAVAILABLE));
    const std::uint64_t total = SizeOf(path);
    std::vector<char> buffer(4 << 20);
    std::uint64_t done = 0;
    for (;;) {
        if (stop.stop_requested()) return Fail(Text::Get(IDS_CANCELED));
        DWORD read = 0;
        if (!ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
            return Fail(ErrorText(Text::Format(IDS_CTX_READ, path.wstring()), GetLastError()));
        if (!read) break;
        hash.Update(buffer.data(), read);
        done += read;
        tracker.Update(done, total);
    }
    return hash.Finish();
}

struct CopyContext {
    Tracker* tracker;
    std::stop_token stop;
};

DWORD CALLBACK CopyProgress(LARGE_INTEGER total, LARGE_INTEGER done, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD,
                            HANDLE, HANDLE, LPVOID data) {
    auto* ctx = static_cast<CopyContext*>(data);
    ctx->tracker->Update(static_cast<std::uint64_t>(done.QuadPart), static_cast<std::uint64_t>(total.QuadPart));
    return ctx->stop.stop_requested() ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
}

// Copies via "<destination>.part" so an interrupted copy never replaces a good file.
Result<> CopyLocal(const fs::path& source, const fs::path& destination, Tracker& tracker, std::stop_token stop) {
    std::error_code ec;
    fs::create_directories(destination.parent_path(), ec);
    fs::path part = destination;
    part += L".part";
    CopyContext ctx{&tracker, stop};
    BOOL cancel = FALSE;
    if (!CopyFileExW(source.c_str(), part.c_str(), CopyProgress, &ctx, &cancel, COPY_FILE_NO_BUFFERING)) {
        const DWORD e = GetLastError();
        fs::remove(part, ec);
        if (e == ERROR_REQUEST_ABORTED || stop.stop_requested()) return Fail(Text::Get(IDS_CANCELED));
        return Fail(ErrorText(Text::Format(IDS_CTX_COPY, source.wstring()), e));
    }
    if (!MoveFileExW(part.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD e = GetLastError();
        fs::remove(part, ec);
        return Fail(ErrorText(Text::Format(IDS_CTX_SAVE, destination.wstring()), e));
    }
    return {};
}

// ---------------------------------------------------------------------------
// Remote files: "up to date" decisions
// ---------------------------------------------------------------------------
// Cache of the last verified digest, so an unchanged 1 GB image is not hashed on every run.
std::wstring VerifiedKey(const fs::path& p, std::wstring_view sha) {
    return std::format(L"{}|{}|{}", SizeOf(p), FileTimeStamp(p), sha);
}

enum class Plan { Skip, Verify, Transfer };

struct Item {
    Sources::Part part;
    std::wstring source;         // URL or local path
    fs::path target;             // where it has to end up (data volume)
    bool local = false;          // source is a local file
    bool useInPlace = false;     // local ESP package: read directly, no copy
    Net::RemoteFileInfo remote;  // URL sources
    std::wstring sha;            // expected digest ("" = unknown)
    std::wstring stamp;          // URL|size|etag|last-modified
    const wchar_t* stampSetting;
    const wchar_t* verifiedSetting;
    std::uint64_t size = 0;      // bytes to transfer
    Plan plan = Plan::Transfer;
    std::wstring name, title;    // translated "image" / "Image", for messages
};

// Where a downloaded package waits until its layout says which folder it belongs to.
fs::path StagingPath() {
    std::error_code ec;
    return fs::temp_directory_path(ec) / std::format(L"LinuxLive-{}-{}", GetCurrentProcessId(), Config::kPackageName);
}

// Moves (or, across volumes, copies) a finished file to its place; the source is gone afterwards.
Result<> MoveInto(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return Fail(ErrorText(Text::Format(IDS_CTX_SAVE, to.wstring()), GetLastError()));
    return {};
}

// Ensures an authorized_keys file exists in the media folder so that headless systems without
// Windows OpenSSH Server can be accessed out of the box via SSH (Brain-Key Pi).
// If the user already provided an authorized_keys file, it is left completely untouched.
void EnsureAuthorizedKeys(const fs::path& mediaDir, const std::function<void(Level, const std::wstring&)>& log) {
    std::error_code ec;
    const fs::path target = mediaDir / L"authorized_keys";
    if (fs::exists(target, ec)) return;

    fs::path temp = target;
    temp += L".tmp";

    constexpr std::string_view kContent =
        "# Linux Live - deterministic emergency SSH key (Brain-Key Pi).\n"
        "# Replace with your own key: ssh-keygen -t ed25519\n"
        "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIIdXmgotoDC4ToQLmyyveTQDpuaV/QLio8d/qPXejYUI\n";

    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out.write(kContent.data(), static_cast<std::streamsize>(kContent.size()));
        out.flush();
        if (!out) {
            out.close();
            fs::remove(temp, ec);
            return;
        }
    }

    if (MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        log(Level::Info, L"SSH: created default authorized_keys (Brain-Key Pi)");
    } else {
        fs::remove(temp, ec);
    }
}

}  // namespace

Result<Install::Layout> Deploy(const Sources::Pair& source, const Storage::EspTarget& chosenEsp, const Hooks& hooks,
                               std::stop_token stop) {
    auto log = [&](Level level, const std::wstring& text) { if (hooks.log) hooks.log(level, text); };
    auto canceled = [&] { return stop.stop_requested(); };
    Tracker tracker(hooks);
    std::error_code ec;

    if (auto r = Sources::Check(source.image, Sources::Part::Image); !r) return Fail(r.error());
    if (auto r = Sources::Check(source.esp, Sources::Part::Package); !r) return Fail(r.error());

    // Local copies of the package deployed before (if any) are where its layout put them.
    const std::optional<Install::Layout> previous = chosenEsp.layout;
    auto before = Storage::LocateData(previous ? &*previous : nullptr);
    if (!before) return Fail(before.error());
    const fs::path previousDir = before->dir;  // empty when nothing was deployed

    Item image{Sources::Part::Image, source.image, previousDir.empty() ? fs::path() : previousDir / Config::kSquashfsName};
    image.stampSetting = L"ImageStamp";
    image.verifiedSetting = L"ImageVerified";
    Item package{Sources::Part::Package, source.esp, previousDir.empty() ? fs::path() : previousDir / Config::kPackageName};
    package.stampSetting = L"EspStamp";
    package.verifiedSetting = L"EspVerified";
    for (Item* item : {&image, &package}) {
        item->name = Sources::PartName(item->part);
        item->title = Capitalize(item->name);
    }

    // Is the local copy at item.target the file the server offers? (item.target empty = no copy)
    auto decide = [&](Item& item) {
        item.plan = Plan::Transfer;
        const std::uint64_t localSize = item.target.empty() ? 0 : SizeOf(item.target);
        if (!localSize) {
            log(Level::Info, Text::Format(IDS_DEPLOY_NO_LOCAL, item.name, item.title, FormatBytes(item.size)));
        } else if (localSize != item.size) {
            log(Level::Info, Text::Format(IDS_DEPLOY_SIZE_DIFFERS, item.name, item.title, FormatBytes(item.size), FormatBytes(localSize)));
        } else if (!item.sha.empty()) {
            // A digest from the server decides, whatever URL the local copy came from.
            if (Settings::Read(item.verifiedSetting) == VerifiedKey(item.target, item.sha)) {
                item.plan = Plan::Skip;
                log(Level::Info, Text::Format(IDS_DEPLOY_VERIFIED_EARLIER, item.name, item.title));
            } else {
                item.plan = Plan::Verify;
            }
        } else {
            const std::wstring stored = Settings::Read(item.stampSetting);
            if (stored.empty() || stored == item.stamp) {
                item.plan = Plan::Skip;
                log(Level::Info, Text::Format(IDS_DEPLOY_CURRENT, item.name, item.title, FormatBytes(localSize)));
            } else {
                log(Level::Info, Text::Format(IDS_DEPLOY_CHANGED, item.name, item.title));
            }
        }
    };

    // --- 1. Inspect the sources -------------------------------------------------
    // The package is compared with its local copy now; the image only once the package has said
    // in which folder it belongs.
    tracker.Indeterminate(Text::Get(IDS_STEP_CHECKING));
    for (Item* item : {&package, &image}) {
        item->local = !Sources::IsUrl(item->source);
        if (item->local) {
            item->size = SizeOf(fs::path(item->source));
            if (item == &package) {
                item->useInPlace = true;  // the tar is only read, so it is used where it is
                item->target = item->source;
                item->plan = Plan::Skip;
                log(Level::Info, Text::Format(IDS_DEPLOY_LOCAL_PACKAGE, item->source, FormatBytes(item->size)));
            }
            continue;
        }
        log(Level::Info, Text::Format(IDS_DEPLOY_CHECKING, item->name, item->title, item->source));
        auto info = Net::CheckRemote(item->source);
        if (!info) return Fail(Text::Format(IDS_DEPLOY_REMOTE_FAILED, item->name, item->title, info.error()));
        if (canceled()) return Fail(Text::Get(IDS_CANCELED));
        item->remote = *info;
        item->size = info->size;
        auto digest = Net::FetchSha256(item->source);
        if (!digest) return Fail(Text::Format(IDS_DEPLOY_CHECKSUM_FAILED, item->name, item->title, digest.error()));
        item->sha = digest->value_or(L"");
        if (item->sha.empty()) {
            if constexpr (Config::kRequireSha256)
                return Fail(Text::Format(IDS_DEPLOY_SHA_REQUIRED, item->name, item->title));
            log(Level::Warning, Text::Format(IDS_DEPLOY_SHA_MISSING, item->name, item->title));
        }
        if (canceled()) return Fail(Text::Get(IDS_CANCELED));
        item->stamp = item->source + L"|" + info->Stamp();
        if (item == &package) decide(*item);
    }

    // --- 2. Steps -----------------------------------------------------------------
    // Weights reflect time: hashing and local copies are much faster than downloads. The image is
    // planned as a transfer until its local copy has been checked (the bar can only jump forward).
    const std::size_t sPackage = tracker.Add(Text::Get(IDS_STEP_DOWNLOAD_PACKAGE), package.plan == Plan::Transfer ? static_cast<double>(package.size) : 0);
    const std::size_t sPackageVerify = tracker.Add(Text::Get(IDS_STEP_VERIFY_PACKAGE), package.plan == Plan::Verify ? 0.1 * static_cast<double>(package.size) : 0);
    const std::size_t sPackageAfterVerify =
        tracker.Add(Text::Get(IDS_STEP_DOWNLOAD_PACKAGE), package.plan == Plan::Verify ? static_cast<double>(package.size) : 0, true);
    const std::size_t sImageVerify = tracker.Add(Text::Get(IDS_STEP_VERIFY_IMAGE), image.local ? 0 : 0.1 * static_cast<double>(image.size), true);
    const std::size_t sImage = tracker.Add(Text::Get(image.local ? IDS_STEP_COPY_IMAGE : IDS_STEP_DOWNLOAD_IMAGE),
                                           (image.local ? 0.25 : 1.0) * static_cast<double>(image.size), true);
    const std::size_t sBootPartition = tracker.Add(Text::Get(IDS_STEP_BOOT_PARTITION), 0.25 * static_cast<double>(std::max<std::uint64_t>(package.size, 1)), true);
    const std::size_t sEsp = tracker.Add(Text::Get(IDS_STEP_WRITE_ESP), 0.25 * static_cast<double>(std::max<std::uint64_t>(package.size, 1)));

    auto verify = [&](Item& item, std::size_t step) -> Result<bool> {
        tracker.Begin(step);
        auto actual = HashFile(item.target, tracker, stop);
        if (!actual) return Fail(actual.error());
        if (*actual == item.sha) {
            Settings::Write(item.verifiedSetting, VerifiedKey(item.target, item.sha));
            Settings::Write(item.stampSetting, item.stamp);
            log(Level::Success, Text::Format(IDS_DEPLOY_MATCHES, item.name, item.title));
            return true;
        }
        log(Level::Info, Text::Format(IDS_DEPLOY_DIFFERS, item.name, item.title));
        return false;
    };
    auto download = [&](Item& item, const fs::path& to, std::size_t step) -> Result<> {
        tracker.Begin(step);
        log(Level::Info, Text::Format(IDS_DEPLOY_DOWNLOADING, item.name, item.title, FormatBytes(item.size), Storage::DisplayPath(to)));
        if (auto r = Net::Download(item.source, to, [&](std::uint64_t d, std::uint64_t t) { tracker.Update(d, t); }, stop, item.sha); !r)
            return r;
        log(Level::Success, Text::Format(item.sha.empty() ? IDS_DEPLOY_SAVED : IDS_DEPLOY_SAVED_VERIFIED, item.name, item.title));
        return {};
    };

    // --- 3. ESP package: a new one waits in the temporary folder for its layout -----------------
    fs::path staged;
    const ScopeExit dropStaged([&] {
        std::error_code ignored;
        if (!staged.empty()) fs::remove(staged, ignored);
    });
    bool fetch = package.plan == Plan::Transfer;
    if (package.plan == Plan::Verify) {
        auto same = verify(package, sPackageVerify);
        if (!same) return Fail(same.error());
        fetch = !*same;
        if (*same) tracker.Skip(sPackageAfterVerify);
    }
    if (fetch) {
        staged = StagingPath();
        if (auto r = download(package, staged, package.plan == Plan::Verify ? sPackageAfterVerify : sPackage); !r) return Fail(r.error());
    }
    const fs::path packageFile = staged.empty() ? package.target : staged;
    if (canceled()) return Fail(Text::Get(IDS_CANCELED));

    // --- 4. Layout: the package names its folders -------------------------------------------
    auto layout = Install::ReadLayout(packageFile);
    if (!layout) return Fail(layout.error());
    Storage::EspTarget esp = chosenEsp;
    Storage::UseLayout(esp, *layout);
    // The ESP folder is fixed by the signed GRUB, so an installed system of the same distribution
    // uses it too (an installed Debian: \EFI\debian). Its boot loader is never replaced.
    if (Install::BelongsToInstalledSystem(esp.dir)) return Fail(Text::Format(IDS_ESP_FOLDER_IN_USE, layout->espFolder));
    auto data = Storage::LocateData(&*layout);
    if (!data) return Fail(data.error());
    log(Level::Info, Text::Format(IDS_DEPLOY_LAYOUT, layout->espFolder, Storage::DisplayPath(data->dir)));
    // What is in the media folder runs as root in Linux: only administrators may change it.
    fs::create_directories(data->dir, ec);
    if (auto r = Storage::ProtectMediaFolder(data->dir); !r) log(Level::Warning, r.error());
    if (previous && !previousDir.empty() && previousDir != data->dir)
        log(Level::Warning, Text::Format(IDS_DEPLOY_MEDIA_MOVED, Storage::DisplayPath(previousDir), Storage::DisplayPath(data->dir)));

    // The package itself is kept in the media folder (the next run compares against it).
    if (!package.useInPlace) {
        const fs::path keep = data->dir / Config::kPackageName;
        if (!staged.empty()) {
            if (auto r = MoveInto(staged, keep); !r) return Fail(r.error());
            staged.clear();
        } else if (package.target != keep) {
            fs::create_directories(keep.parent_path(), ec);
            if (!CopyFileW(package.target.c_str(), keep.c_str(), FALSE)) return Fail(ErrorText(Text::Format(IDS_CTX_COPY, keep.wstring()), GetLastError()));
        }
        package.target = keep;
        Settings::Write(package.stampSetting, package.stamp);
        if (!package.sha.empty()) Settings::Write(package.verifiedSetting, VerifiedKey(package.target, package.sha));
    }

    // --- 5. Image: compared with the copy in this package's media folder --------------------
    image.target = data->dir / Config::kSquashfsName;
    if (image.local) {
        const fs::path src(image.source);
        if (fs::equivalent(src, image.target, ec)) {
            image.plan = Plan::Skip;
            log(Level::Info, Text::Get(IDS_DEPLOY_LOCAL_SAME));
        } else if (SizeOf(image.target) == image.size && FileTimeStamp(image.target) == FileTimeStamp(src)) {
            image.plan = Plan::Skip;
            log(Level::Info, Text::Format(IDS_DEPLOY_LOCAL_CURRENT, FormatBytes(image.size), image.source));
        } else {
            image.plan = Plan::Transfer;
            log(Level::Info, Text::Format(IDS_DEPLOY_LOCAL_COPY, image.source, FormatBytes(image.size)));
        }
    } else {
        decide(image);
    }
    if (image.plan == Plan::Skip) {
        tracker.Skip(sImageVerify);
        tracker.Skip(sImage);
    } else if (image.plan == Plan::Transfer) {
        tracker.Skip(sImageVerify);
    }

    // The old image stays until the new one is complete, so its full size must be free.
    ULARGE_INTEGER freeBytes{};
    if (image.plan != Plan::Skip && GetDiskFreeSpaceExW(data->volume.root.c_str(), &freeBytes, nullptr, nullptr) &&
        image.size > freeBytes.QuadPart)
        return Fail(Text::Format(IDS_DEPLOY_NO_SPACE, data->volume.Name(), FormatBytes(image.size), FormatBytes(freeBytes.QuadPart)));

    if (image.plan == Plan::Verify) {
        auto same = verify(image, sImageVerify);
        if (!same) return Fail(same.error());
        image.plan = *same ? Plan::Skip : Plan::Transfer;
        if (*same) tracker.Skip(sImage);
    }
    if (image.plan == Plan::Transfer) {
        if (image.local) {
            tracker.Begin(sImage);
            log(Level::Info, Text::Format(IDS_DEPLOY_COPYING, image.source, Storage::DisplayPath(image.target)));
            if (auto r = CopyLocal(image.source, image.target, tracker, stop); !r) return Fail(r.error());
            log(Level::Success, Text::Get(IDS_DEPLOY_COPIED));
        } else {
            if (auto r = download(image, image.target, sImage); !r) return Fail(r.error());
            Settings::Write(image.stampSetting, image.stamp);
            if (!image.sha.empty()) Settings::Write(image.verifiedSetting, VerifiedKey(image.target, image.sha));
        }
    }
    if (canceled()) return Fail(Text::Get(IDS_CANCELED));

    // --- 6. ESP -------------------------------------------------------------------------
    // An ESP too small for the kernel and initrd (older installations have 100 MB) keeps only the
    // signed boot loader; kernel and initrd go to a boot partition cut from the Windows volume, which
    // is only created after a yes. The firmware update reserve on the ESP is never touched.
    const Install::Destination espTarget{esp.volume.root, esp.dir, Config::EspReserve(esp.partition.sizeBytes)};
    auto plan = Install::PlanEsp(package.target, espTarget, layout->espFolder);
    if (!plan) return Fail(plan.error());
    std::optional<Install::Destination> kernelTarget;
    if (plan->FitsAll()) {
        tracker.Skip(sBootPartition);
    } else {
        if (!plan->FitsLoader())
            return Fail(Text::Format(IDS_ESP_NO_SPACE_RESERVE, FormatBytes(plan->loaderGrowth), FormatBytes(plan->reserve),
                                     FormatBytes(plan->freeBytes + plan->kernelOnEsp)));
        if (!layout->kernelSearch) return Fail(Text::Get(IDS_PACKAGE_NO_SPLIT));
        auto boot = Storage::FindBootPartition();
        if (boot) {
            tracker.Skip(sBootPartition);
        } else {
            const std::wstring question =
                Text::Format(IDS_BOOTPART_QUESTION, FormatBytes(plan->freeBytes + plan->kernelOnEsp), FormatBytes(plan->reserve),
                             FormatBytes(plan->kernelBytes), data->volume.Name(), FormatBytes(Config::kBootPartitionBytes));
            if (!hooks.confirmBootPartition || !hooks.confirmBootPartition(question)) return Fail(Text::Get(IDS_BOOTPART_DECLINED));
            tracker.Begin(sBootPartition);
            auto created = Storage::CreateBootPartition(data->volume, Config::kBootPartitionBytes,
                                                        [&](std::wstring line) { log(Level::Info, line); });
            if (!created) return Fail(created.error());
            boot = std::move(*created);
        }
        kernelTarget = Install::Destination{boot->volume.root, fs::path(boot->volume.root) / layout->mediaFolder, 0};
        log(Level::Info, Text::Format(IDS_DEPLOY_SPLIT, layout->espFolder, FormatBytes(boot->size), boot->disk, boot->number));
    }
    tracker.Begin(sEsp);
    auto report = Install::DeployEsp(package.target, espTarget, kernelTarget ? &*kernelTarget : nullptr, layout->espFolder,
                                     [&](std::uint64_t d, std::uint64_t t) { tracker.Update(d, t); },
                                     [&](std::wstring line) { log(Level::Info, line); }, stop);
    if (!report) return Fail(report.error());
    log(report->loaderPath.empty() ? Level::Warning : Level::Success,
        Text::Format(IDS_DEPLOY_ESP_RESULT, report->files, FormatBytes(report->bytes), report->skipped,
                     report->loaderPath.empty() ? Text::Get(IDS_DEPLOY_NO_LOADER) : report->loaderPath));

    // --- 7. BitLocker ---------------------------------------------------------------
    // The recovery password only goes to the (unencrypted) ESP after an explicit yes.
    if (auto r = Storage::SyncBitLockerKey(data->volume.root, data->volume.Name(), esp.dir,
                                           [&](std::wstring line) { log(Level::Info, line); }, hooks.confirm);
        !r)
        log(Level::Warning, Text::Format(IDS_DEPLOY_BITLOCKER_ERROR, r.error()));

    // --- 8. SSH authorized_keys ----------------------------------------------------
    // Seeded with the deterministic Brain-Key Pi public key so headless systems without
    // Windows OpenSSH can be accessed out of the box. Never overwrites an existing file.
    EnsureAuthorizedKeys(data->dir, log);

    return *layout;
}

}  // namespace DL::Core
