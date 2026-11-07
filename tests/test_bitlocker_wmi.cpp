#include <Windows.h>
#include <wbemidl.h>
#include <comdef.h>
#include <cstdio>
#include <string>
#include <optional>
#include <vector>

#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

std::optional<std::string> GetBitLockerKeyForVolume(const std::wstring& driveOrDevice) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool needUninit = SUCCEEDED(hr);

    IWbemLocator* pLoc = nullptr;
    hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, reinterpret_cast<void**>(&pLoc));
    if (FAILED(hr)) {
        if (needUninit) CoUninitialize();
        return std::nullopt;
    }

    IWbemServices* pSvc = nullptr;
    hr = pLoc->ConnectServer(_bstr_t(L"ROOT\\CIMV2\\Security\\MicrosoftVolumeEncryption"), nullptr, nullptr, nullptr, 0, nullptr, nullptr, &pSvc);
    pLoc->Release();
    if (FAILED(hr)) {
        if (needUninit) CoUninitialize();
        return std::nullopt;
    }

    hr = CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                           RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    if (FAILED(hr)) {
        pSvc->Release();
        if (needUninit) CoUninitialize();
        return std::nullopt;
    }

    IEnumWbemClassObject* pEnum = nullptr;
    hr = pSvc->ExecQuery(_bstr_t(L"WQL"), _bstr_t(L"SELECT * FROM Win32_EncryptableVolume"),
                         WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnum);
    if (FAILED(hr)) {
        pSvc->Release();
        if (needUninit) CoUninitialize();
        return std::nullopt;
    }

    IWbemClassObject* pObj = nullptr;
    ULONG uReturn = 0;
    std::optional<std::string> resultKey;

    while (pEnum->Next(WBEM_INFINITE, 1, &pObj, &uReturn) == S_OK && uReturn > 0) {
        VARIANT vtDrive, vtDevice;
        VariantInit(&vtDrive);
        VariantInit(&vtDevice);
        pObj->Get(L"DriveLetter", 0, &vtDrive, nullptr, nullptr);
        pObj->Get(L"DeviceID", 0, &vtDevice, nullptr, nullptr);

        bool match = false;
        if (vtDrive.vt == VT_BSTR && vtDrive.bstrVal) {
            if (_wcsicmp(vtDrive.bstrVal, driveOrDevice.c_str()) == 0) match = true;
        }
        if (!match && vtDevice.vt == VT_BSTR && vtDevice.bstrVal) {
            std::wstring dev(vtDevice.bstrVal);
            std::wstring target(driveOrDevice);
            while (!dev.empty() && dev.back() == L'\\') dev.pop_back();
            while (!target.empty() && target.back() == L'\\') target.pop_back();
            if (_wcsicmp(dev.c_str(), target.c_str()) == 0) match = true;
        }
        VariantClear(&vtDrive);
        VariantClear(&vtDevice);

        if (!match) {
            pObj->Release();
            continue;
        }

        // GetProtectionStatus
        IWbemClassObject* pOutStatus = nullptr;
        VARIANT vtPath;
        VariantInit(&vtPath);
        pObj->Get(L"__RELPATH", 0, &vtPath, nullptr, nullptr);

        hr = pSvc->ExecMethod(vtPath.bstrVal, _bstr_t(L"GetProtectionStatus"), 0, nullptr, nullptr, &pOutStatus, nullptr);
        if (SUCCEEDED(hr) && pOutStatus) {
            VARIANT vtStatus;
            VariantInit(&vtStatus);
            pOutStatus->Get(L"ProtectionStatus", 0, &vtStatus, nullptr, nullptr);
            const uint32_t status = (vtStatus.vt == VT_I4 || vtStatus.vt == VT_UI4) ? vtStatus.uintVal : 0;
            VariantClear(&vtStatus);
            pOutStatus->Release();

            std::printf("ProtectionStatus = %u\n", status);
            if (status != 1) { // 1 = PROTECTION ON
                VariantClear(&vtPath);
                pObj->Release();
                break;
            }
        }

        // GetKeyProtectors(KeyProtectorType = 3)
        IWbemClassObject* pClass = nullptr;
        pSvc->GetObject(_bstr_t(L"Win32_EncryptableVolume"), 0, nullptr, &pClass, nullptr);
        IWbemClassObject* pInParamsDef = nullptr;
        if (pClass) {
            pClass->GetMethod(L"GetKeyProtectors", 0, &pInParamsDef, nullptr);
            pClass->Release();
        }

        IWbemClassObject* pInParams = nullptr;
        if (pInParamsDef) {
            pInParamsDef->SpawnInstance(0, &pInParams);
            pInParamsDef->Release();
        }

        if (pInParams) {
            VARIANT vtType;
            vtType.vt = VT_I4;
            vtType.lVal = 3; // 3 = Numerical Password
            pInParams->Put(L"KeyProtectorType", 0, &vtType, 0);

            IWbemClassObject* pOutKeys = nullptr;
            hr = pSvc->ExecMethod(vtPath.bstrVal, _bstr_t(L"GetKeyProtectors"), 0, nullptr, pInParams, &pOutKeys, nullptr);
            pInParams->Release();

            if (SUCCEEDED(hr) && pOutKeys) {
                VARIANT vtIDs;
                VariantInit(&vtIDs);
                pOutKeys->Get(L"VolumeKeyProtectorID", 0, &vtIDs, nullptr, nullptr);

                if (vtIDs.vt == (VT_ARRAY | VT_BSTR) && vtIDs.parray) {
                    BSTR* pBstr = nullptr;
                    SafeArrayAccessData(vtIDs.parray, reinterpret_cast<void**>(&pBstr));
                    LONG lBound = 0, uBound = 0;
                    SafeArrayGetLBound(vtIDs.parray, 1, &lBound);
                    SafeArrayGetUBound(vtIDs.parray, 1, &uBound);

                    if (uBound >= lBound && pBstr) {
                        _bstr_t protectorId(pBstr[0]);
                        SafeArrayUnaccessData(vtIDs.parray);

                        // Call GetKeyProtectorNumericalPassword
                        IWbemClassObject* pClass2 = nullptr;
                        pSvc->GetObject(_bstr_t(L"Win32_EncryptableVolume"), 0, nullptr, &pClass2, nullptr);
                        IWbemClassObject* pInPassDef = nullptr;
                        if (pClass2) {
                            pClass2->GetMethod(L"GetKeyProtectorNumericalPassword", 0, &pInPassDef, nullptr);
                            pClass2->Release();
                        }
                        IWbemClassObject* pInPass = nullptr;
                        if (pInPassDef) {
                            pInPassDef->SpawnInstance(0, &pInPass);
                            pInPassDef->Release();
                        }
                        if (pInPass) {
                            VARIANT vtIDVal;
                            vtIDVal.vt = VT_BSTR;
                            vtIDVal.bstrVal = protectorId;
                            pInPass->Put(L"VolumeKeyProtectorID", 0, &vtIDVal, 0);

                            IWbemClassObject* pOutPass = nullptr;
                            hr = pSvc->ExecMethod(vtPath.bstrVal, _bstr_t(L"GetKeyProtectorNumericalPassword"), 0, nullptr, pInPass, &pOutPass, nullptr);
                            pInPass->Release();

                            if (SUCCEEDED(hr) && pOutPass) {
                                VARIANT vtPass;
                                VariantInit(&vtPass);
                                pOutPass->Get(L"NumericalPassword", 0, &vtPass, nullptr, nullptr);
                                if (vtPass.vt == VT_BSTR && vtPass.bstrVal) {
                                    _bstr_t bPass(vtPass.bstrVal);
                                    resultKey = static_cast<const char*>(bPass);
                                }
                                VariantClear(&vtPass);
                                pOutPass->Release();
                            }
                        }
                    } else {
                        SafeArrayUnaccessData(vtIDs.parray);
                    }
                }
                VariantClear(&vtIDs);
                pOutKeys->Release();
            }
        }
        VariantClear(&vtPath);
        pObj->Release();
        break;
    }

    pEnum->Release();
    pSvc->Release();
    if (needUninit) CoUninitialize();
    return resultKey;
}

int main() {
    auto key = GetBitLockerKeyForVolume(L"C:");
    if (key) {
        std::printf("Found BitLocker key: %s (len: %zu)\n", key->c_str(), key->size());
    } else {
        std::printf("No BitLocker key found or volume not encrypted (ProtectionStatus = 0).\n");
    }
    return 0;
}
