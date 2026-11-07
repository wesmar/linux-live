#include "storage/BitLocker.h"
#include "support/Text.h"
#include "support/Wmi.h"
#include <Windows.h>
#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <vector>

namespace DL::Storage {
namespace fs = std::filesystem;
using namespace Wmi;

namespace {

constexpr wchar_t kClass[] = L"Win32_EncryptableVolume";

// ---------------------------------------------------------------------------
// Misc
// ---------------------------------------------------------------------------
std::wstring TrimBackslashes(std::wstring s) {
    while (!s.empty() && s.back() == L'\\') s.pop_back();
    return s;
}

bool SameVolume(const std::wstring& a, const std::wstring& b) {
    return !a.empty() && !b.empty() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

std::string TrimAscii(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

// Reads the first numerical recovery password of the volume, or nullopt.
std::optional<std::string> ReadRecoveryPassword(const Service& svc, const std::wstring& path) {
    auto in = svc.InParams(kClass, L"GetKeyProtectors");
    if (!in || !*in || FAILED(PutU32(in->get(), L"KeyProtectorType", 3))) return std::nullopt;  // 3 = Numerical Password
    auto out = svc.Call(path, L"GetKeyProtectors", in->get());
    if (!out || !*out || ReturnValue(out->get()) != 0) return std::nullopt;

    for (const auto& id : GetStrings(out->get(), L"VolumeKeyProtectorID")) {
        auto passIn = svc.InParams(kClass, L"GetKeyProtectorNumericalPassword");
        if (!passIn || !*passIn) return std::nullopt;
        if (FAILED(PutString(passIn->get(), L"VolumeKeyProtectorID", id))) continue;
        auto passOut = svc.Call(path, L"GetKeyProtectorNumericalPassword", passIn->get());
        if (!passOut || !*passOut || ReturnValue(passOut->get()) != 0) continue;
        Variant password;
        if (FAILED((*passOut)->Get(L"NumericalPassword", 0, &password.v, nullptr, nullptr))) continue;
        if (password.v.vt != VT_BSTR || !password.v.bstrVal) continue;

        std::string key;
        const UINT length = SysStringLen(password.v.bstrVal);
        for (UINT i = 0; i < length; ++i) {
            const wchar_t c = password.v.bstrVal[i];
            if (c < 0x80) key.push_back(static_cast<char>(c));
        }
        SecureZeroMemory(password.v.bstrVal, SysStringByteLen(password.v.bstrVal));  // don't leave it in memory
        key = TrimAscii(std::move(key));
        if (!key.empty()) return key;
    }
    return std::nullopt;
}

}  // namespace

Result<BitLockerStatus> QueryBitLocker(const std::wstring& volume) {
    ComScope com;
    auto svc = Service::Connect(L"ROOT\\CIMV2\\Security\\MicrosoftVolumeEncryption");
    if (!svc) return Fail(svc.error());
    auto volumes = svc->Query(L"SELECT * FROM Win32_EncryptableVolume");
    if (!volumes) return Fail(volumes.error());

    // "C:" and "C:\" are the same volume; so are "\\?\Volume{..}" and "\\?\Volume{..}\".
    const std::wstring wanted = TrimBackslashes(volume);
    for (const auto& object : *volumes) {
        const std::wstring letter = TrimBackslashes(GetString(object.get(), L"DriveLetter"));
        const std::wstring device = TrimBackslashes(GetString(object.get(), L"DeviceID"));
        if (!SameVolume(wanted, letter) && !SameVolume(wanted, device)) continue;

        const std::wstring path = GetString(object.get(), L"__RELPATH");
        if (path.empty()) return Fail(Text::Get(IDS_BL_NO_OBJECT));

        BitLockerStatus result;
        std::uint32_t protection = 2;  // 2 = unknown (what a locked volume reports)
        if (auto out = svc->Call(path, L"GetProtectionStatus", nullptr); out && *out)
            if (const auto value = GetU32(out->get(), L"ProtectionStatus")) {
                protection = *value;
                result.protectionOn = protection == 1;
            }

        // "Protection Off" does not mean "not encrypted" (suspended volumes are still encrypted),
        // so the conversion state decides.
        std::optional<std::uint32_t> conversion;  // 0 = fully decrypted
        if (auto in = svc->InParams(kClass, L"GetConversionStatus"); in && *in && SUCCEEDED(PutU32(in->get(), L"PrecisionFactor", 0)))
            if (auto out = svc->Call(path, L"GetConversionStatus", in->get()); out && *out && ReturnValue(out->get()) == 0)
                conversion = GetU32(out->get(), L"ConversionStatus");

        if (conversion)
            result.state = *conversion == 0 ? BitLockerStatus::State::NotEncrypted : BitLockerStatus::State::Encrypted;
        else if (protection == 1)
            result.state = BitLockerStatus::State::Encrypted;
        else
            result.state = BitLockerStatus::State::Unknown;

        if (result.state == BitLockerStatus::State::Encrypted) result.key = ReadRecoveryPassword(*svc, path);
        return result;
    }
    return Fail(Text::Get(IDS_BL_NOT_FOUND));
}

Result<> SyncBitLockerKey(const std::wstring& volumeRoot, const std::wstring& displayName, const fs::path& espDir,
                          const LogFn& log, const ConfirmFn& confirm) {
    const fs::path keyTarget = espDir / L"bitlocker.key";
    std::error_code ec;

    auto status = QueryBitLocker(volumeRoot);
    if (!status) {
        log(Text::Format(IDS_BL_SKIPPED, status.error()));
        return {};
    }

    switch (status->state) {
    case BitLockerStatus::State::NotEncrypted:
        if (fs::exists(keyTarget, ec)) {
            if (fs::remove(keyTarget, ec)) log(Text::Format(IDS_BL_REMOVED_STALE, displayName));
            else log(Text::Format(IDS_BL_STALE_LEFT, displayName, keyTarget.wstring()));
        } else {
            log(Text::Format(IDS_BL_NOT_ENCRYPTED, displayName));
        }
        return {};
    case BitLockerStatus::State::Unknown:
        log(Text::Format(IDS_BL_UNKNOWN, displayName));
        return {};
    case BitLockerStatus::State::Encrypted:
        break;
    }

    if (!status->key || status->key->empty()) {
        log(Text::Format(IDS_BL_NO_PASSWORD, displayName));
        return {};
    }
    const std::string& newKey = *status->key;

    // Up to date? Then there is nothing to write and nothing to ask.
    if (fs::is_regular_file(keyTarget, ec)) {
        std::ifstream currentFile(keyTarget, std::ios::binary);
        if (currentFile) {
            std::string currentKey((std::istreambuf_iterator<char>(currentFile)), std::istreambuf_iterator<char>());
            if (TrimAscii(currentKey) == newKey) {
                log(Text::Format(IDS_BL_UP_TO_DATE, newKey.size()));
                return {};
            }
        }
    }

    const std::wstring warning = Text::Format(IDS_BL_QUESTION, displayName, keyTarget.wstring());
    if (!confirm || !confirm(warning)) {
        log(Text::Get(IDS_BL_DECLINED));
        return {};
    }

    // Write atomically.
    fs::create_directories(espDir, ec);
    fs::path tempPath = keyTarget;
    tempPath += L".tmp";
    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out) return Fail(Text::Format(IDS_CANNOT_CREATE, tempPath.wstring()));
        out.write(newKey.data(), static_cast<std::streamsize>(newKey.size()));
        out.flush();
        if (!out) {
            out.close();
            fs::remove(tempPath, ec);
            return Fail(Text::Format(IDS_CANNOT_WRITE, tempPath.wstring()));
        }
    }
    if (!MoveFileExW(tempPath.c_str(), keyTarget.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD e = GetLastError();
        fs::remove(tempPath, ec);
        return Fail(ErrorText(Text::Format(IDS_CTX_REPLACE, keyTarget.wstring()), e));
    }

    log(Text::Format(IDS_BL_SAVED, keyTarget.wstring(), newKey.size()));
    return {};
}

}  // namespace DL::Storage
