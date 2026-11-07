#include "efi/Bcd.h"
#include "support/Wmi.h"
#include <cwctype>
#include <format>
#include <optional>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace DL::Efi::Bcd {
namespace {

using namespace Wmi;

constexpr std::uint32_t kTypeBootManager = 0x10100002;     // Windows Boot Manager (and copies of it)
constexpr std::uint32_t kTypeFirmwareApp = 0x101FFFFF;     // firmware application (an imported NVRAM entry)
constexpr std::uint32_t kElementPath = 0x12000002;         // BcdLibraryString_ApplicationPath
constexpr std::uint32_t kElementDescription = 0x12000004;  // BcdLibraryString_Description
constexpr wchar_t kStorePath[] = L"BcdStore.FilePath=\"\"";  // the system store
constexpr wchar_t kBootMgr[] = L"{9dea862c-5cdd-4e70-acc1-f32b344d4795}";
constexpr wchar_t kFwBootMgr[] = L"{a5a30fa2-3d06-4e9f-b5f4-a01df9d1fcba}";

bool SameText(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) { return std::towlower(x) == std::towlower(y); });
}

std::wstring Normalized(std::wstring path) {
    for (auto& c : path)
        if (c == L'/') c = L'\\';
    if (!path.empty() && path.front() != L'\\') path.insert(path.begin(), L'\\');
    return path;
}

// BCD methods report success through a boolean ReturnValue.
bool Succeeded(IWbemClassObject* out) { return out && GetBool(out, L"ReturnValue").value_or(false); }

std::wstring ObjectPath(const std::wstring& id) { return std::format(L"BcdObject.Id=\"{}\",StoreFilePath=\"\"", id); }

class Store {
public:
    static Result<Store> Open() {
        // bcdedit needs these as well; an elevated process holds them and only has to enable them.
        (void)EnablePrivilege(SE_BACKUP_NAME);
        (void)EnablePrivilege(SE_RESTORE_NAME);
        auto svc = Service::Connect(L"ROOT\\WMI");
        if (!svc) return Fail(svc.error());
        Store store(std::move(*svc));
        auto in = store.m_svc.InParams(L"BcdStore", L"OpenStore");
        if (!in) return Fail(in.error());
        PutString(in->get(), L"File", L"");
        auto out = store.m_svc.Call(L"BcdStore", L"OpenStore", in->get());
        if (!out) return Fail(out.error());
        if (!Succeeded(out->get())) return Fail(L"BcdStore.OpenStore: the system BCD store cannot be opened.");
        return store;
    }

    Result<std::vector<std::wstring>> Objects(std::uint32_t type) const {
        auto in = m_svc.InParams(L"BcdStore", L"EnumerateObjects");
        if (!in) return Fail(in.error());
        PutU32(in->get(), L"Type", type);
        auto out = m_svc.Call(kStorePath, L"EnumerateObjects", in->get());
        if (!out) return Fail(out.error());
        std::vector<std::wstring> ids;
        if (!Succeeded(out->get())) return ids;
        for (const auto& object : GetEmbeddedArray(out->get(), L"Objects")) ids.push_back(GetString(object.get(), L"Id"));
        return ids;
    }

    std::wstring Text(const std::wstring& id, std::uint32_t element) const {
        auto in = m_svc.InParams(L"BcdObject", L"GetElement");
        if (!in) return {};
        PutU32(in->get(), L"Type", element);
        auto out = m_svc.Call(ObjectPath(id), L"GetElement", in->get());
        if (!out || !Succeeded(out->get())) return {};
        return GetString(GetEmbedded(out->get(), L"Element").get(), L"String");
    }

    Result<> Delete(const std::wstring& id) const {
        auto in = m_svc.InParams(L"BcdStore", L"DeleteObject");
        if (!in) return Fail(in.error());
        PutString(in->get(), L"Id", id);
        auto out = m_svc.Call(kStorePath, L"DeleteObject", in->get());
        if (!out) return Fail(out.error());
        if (!Succeeded(out->get())) return Fail(std::format(L"BcdStore.DeleteObject {} failed.", id));
        return {};
    }

private:
    explicit Store(Service svc) : m_svc(std::move(svc)) {}
    Service m_svc;
};

// ---------------------------------------------------------------------------------------------
// Reading the BCD store from its registry hive.
//
// Over WMI every BcdObject call re-opens the system store (~300 ms each, measured), and a Windows
// BCD has dozens of objects. The system store is mounted as the hive HKLM\BCD00000000, readable
// by SYSTEM only; with SeBackupPrivilege and REG_OPTION_BACKUP_RESTORE an administrator opens it
// anyway (the same access bcdedit gets through its own privileges). All objects are read locally in
// a few milliseconds. The hive is only read; deleting still goes through WMI. Any failure here makes
// the caller fall back to reading over WMI.
// ---------------------------------------------------------------------------------------------
struct Item {
    std::wstring id, path, description;
    std::uint32_t type = 0;
};

struct RegKey {
    HKEY h = nullptr;
    RegKey() = default;
    RegKey(const RegKey&) = delete;
    RegKey& operator=(const RegKey&) = delete;
    ~RegKey() {
        if (h) RegCloseKey(h);
    }
};

// Subkeys are opened one by one with the backup flag: the access check of a relative open does not
// inherit the privilege from the parent handle.
bool OpenBackup(HKEY parent, const std::wstring& subkey, RegKey& key) {
    return RegOpenKeyExW(parent, subkey.c_str(), REG_OPTION_BACKUP_RESTORE, KEY_READ, &key.h) == ERROR_SUCCESS;
}

std::wstring HiveString(HKEY parent, const std::wstring& subkey) {
    RegKey key;
    if (!OpenBackup(parent, subkey, key)) return {};
    DWORD bytes = 0;
    if (RegGetValueW(key.h, nullptr, L"Element", RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes < sizeof(wchar_t)) return {};
    std::wstring text(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(key.h, nullptr, L"Element", RRF_RT_REG_SZ, nullptr, text.data(), &bytes) != ERROR_SUCCESS) return {};
    while (!text.empty() && text.back() == L'\0') text.pop_back();
    return text;
}

// All objects of the system store with their type, loader path and description; false when the
// hive cannot be opened (or holds no objects at all).
bool ReadHive(std::vector<Item>& out) {
    (void)EnablePrivilege(SE_BACKUP_NAME);
    RegKey objects;
    if (!OpenBackup(HKEY_LOCAL_MACHINE, L"BCD00000000\\Objects", objects)) return false;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256]{};
        DWORD length = 256;
        const LSTATUS status = RegEnumKeyExW(objects.h, i, name, &length, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS) { out.clear(); return false; }
        Item item;
        item.id.assign(name, length);
        if (RegKey description; OpenBackup(objects.h, item.id + L"\\Description", description)) {
            DWORD type = 0, size = sizeof type;
            if (RegGetValueW(description.h, nullptr, L"Type", RRF_RT_REG_DWORD, nullptr, &type, &size) == ERROR_SUCCESS) item.type = type;
        }
        item.path = HiveString(objects.h, item.id + L"\\Elements\\12000002");         // ApplicationPath
        item.description = HiveString(objects.h, item.id + L"\\Elements\\12000004");  // Description
        out.push_back(std::move(item));
    }
    return !out.empty();
}

}  // namespace

Result<Removal> RemoveCopies(const BootEntry& entry, const std::function<bool(const Copy&)>& sharedElsewhere) {
    ComScope com;
    const std::wstring path = Normalized(entry.filePath);
    Removal result;
    std::optional<Store> store;  // opened on first need: with the hive read, often never

    auto ensureStore = [&]() -> Result<> {
        if (store) return {};
        auto opened = Store::Open();
        if (!opened) return Fail(opened.error());
        store.emplace(std::move(*opened));
        return {};
    };

    // Decides about one BCD object and deletes it when it is a copy of `entry`.
    auto consider = [&](const std::wstring& id, std::wstring objectPath, const std::function<std::wstring()>& description) -> Result<> {
        if (SameText(id, kBootMgr) || SameText(id, kFwBootMgr)) return {};
        objectPath = Normalized(std::move(objectPath));
        if (SameText(objectPath, L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi")) return {};
        const bool named = !entry.bcdObject.empty() && SameText(id, entry.bcdObject);
        if (!named && (path.empty() || !SameText(objectPath, path))) return {};
        Copy copy{id, description(), std::move(objectPath)};
        if (!named && copy.description != entry.description) return {};
        if (!named && sharedElsewhere && sharedElsewhere(copy)) return {};
        if (auto r = ensureStore(); !r) return Fail(r.error());
        if (auto r = store->Delete(id); r) result.removed.push_back(id);
        else result.warning = r.error();
        return {};
    };

    std::vector<Item> items;
    if (ReadHive(items)) {
        for (const auto& item : items) {
            if (item.type != kTypeFirmwareApp && item.type != kTypeBootManager) continue;
            if (auto r = consider(item.id, item.path, [&] { return item.description; }); !r) return Fail(r.error());
        }
    } else {
        if (auto r = ensureStore(); !r) return Fail(r.error());
        for (const std::uint32_t type : {kTypeFirmwareApp, kTypeBootManager}) {
            auto ids = store->Objects(type);
            if (!ids) return Fail(ids.error());
            for (const auto& id : *ids) {
                if (auto r = consider(id, store->Text(id, kElementPath), [&] { return store->Text(id, kElementDescription); }); !r)
                    return Fail(r.error());
            }
        }
    }
    return result;
}

}  // namespace DL::Efi::Bcd
