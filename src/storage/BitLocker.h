// Native WinAPI (WMI in-process) BitLocker detection and recovery key management.
#pragma once
#include "support/Common.h"
#include <filesystem>
#include <optional>
#include <string>

namespace DL::Storage {

struct BitLockerStatus {
    enum class State {
        NotEncrypted,  // fully decrypted
        Encrypted,     // encrypted (also in progress / paused / protection suspended)
        Unknown,       // state could not be determined
    };
    State state = State::Unknown;
    bool protectionOn = false;       // ProtectionStatus == 1 (as opposed to suspended)
    std::optional<std::string> key;  // 55-character Numerical Password (8 groups of 6 digits)
};

// Inspects BitLocker on a volume. `volume` is a volume GUID path ("\\?\Volume{...}\", preferred -
// it always works) or a drive letter ("C:" / "C:\"). Pure in-process COM/WMI, no external tools.
Result<BitLockerStatus> QueryBitLocker(const std::wstring& volume);

// Keeps <espDir>\bitlocker.key in step with the data volume:
//  - encrypted volume: the recovery password is written to the ESP ONLY after `confirm` says yes,
//    because the ESP is not encrypted (anyone with physical access can read the file);
//    an up-to-date file is left alone and nothing is asked;
//  - fully decrypted volume: a stale bitlocker.key is deleted;
//  - unknown state (e.g. locked volume): the ESP is left unchanged.
Result<> SyncBitLockerKey(const std::wstring& volumeRoot, const std::wstring& displayName,
                          const std::filesystem::path& espDir, const LogFn& log, const ConfirmFn& confirm);

}  // namespace DL::Storage
