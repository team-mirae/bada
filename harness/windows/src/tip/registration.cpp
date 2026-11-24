// Adapted from bada-windows src/tip/registration.cpp (jis-moonshot's).
#include "registration.h"

#include <msctf.h>
#include <olectl.h>
#include <wrl/client.h>

#include <iterator>
#include <string>

#include "guids.h"

using Microsoft::WRL::ComPtr;

namespace bada::tip {
namespace {

// As Mozc and jis-moonshot register. Immersive support lets Store apps load it; UI-less, it runs
// in every kind of app.
const GUID* const kCategories[] = {
    &GUID_TFCAT_TIP_KEYBOARD,
    &GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER,
    &GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT,
    &GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT,
    &GUID_TFCAT_TIPCAP_UIELEMENTENABLED,
    &GUID_TFCAT_TIPCAP_COMLESS,
    &GUID_TFCAT_TIPCAP_INPUTMODECOMPARTMENT,
};

std::wstring ClsidKeyPath() { return std::wstring(L"Software\\Classes\\CLSID\\") + kClsidText; }

}  // namespace

HRESULT RegisterServer(HINSTANCE module) {
    wchar_t dll[MAX_PATH];
    const DWORD length = GetModuleFileNameW(module, dll, MAX_PATH);
    if (!length || length == MAX_PATH) return E_FAIL;
    HKEY clsid = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, ClsidKeyPath().c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &clsid,
                        nullptr) != ERROR_SUCCESS) {
        return E_ACCESSDENIED;
    }
    RegSetValueExW(clsid, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(kDisplayName), sizeof(kDisplayName));
    HKEY server = nullptr;
    LSTATUS status = RegCreateKeyExW(clsid, L"InprocServer32", 0, nullptr, 0, KEY_WRITE, nullptr, &server, nullptr);
    if (status == ERROR_SUCCESS) {
        constexpr wchar_t kApartment[] = L"Apartment";
        status = RegSetValueExW(server, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(dll),
                                (length + 1) * sizeof(wchar_t));
        if (status == ERROR_SUCCESS) {
            status = RegSetValueExW(server, L"ThreadingModel", 0, REG_SZ, reinterpret_cast<const BYTE*>(kApartment),
                                    sizeof(kApartment));
        }
        RegCloseKey(server);
    }
    RegCloseKey(clsid);
    return HRESULT_FROM_WIN32(status);
}

HRESULT UnregisterServer() {
    const LSTATUS status = RegDeleteTreeW(HKEY_LOCAL_MACHINE, ClsidKeyPath().c_str());
    return status == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(status);
}

HRESULT RegisterProfiles(HINSTANCE module) {
    ComPtr<ITfInputProcessorProfileMgr> manager;
    HRESULT hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&manager));
    if (FAILED(hr)) return hr;
    ComPtr<ITfInputProcessorProfiles> profiles;
    hr = manager.As(&profiles);
    if (FAILED(hr)) return hr;
    // The TIP-level "Enable" value: Windows Settings lists a TIP's keyboards only with it.
    hr = profiles->Register(kClsid);
    if (FAILED(hr)) return hr;
    // The icon is the DLL's own, the first one.
    wchar_t dll[MAX_PATH];
    const DWORD length = GetModuleFileNameW(module, dll, MAX_PATH);
    // Registered, the profile is offered under Japanese in Windows' settings. It is in nobody's
    // list of keyboards until they add it there, and it is never made the default.
    return manager->RegisterProfile(kClsid, kLanguage, kProfile, kDisplayName,
                                    static_cast<ULONG>(std::size(kDisplayName) - 1), dll, length, 0, nullptr, 0, TRUE,
                                    0);
}

HRESULT UnregisterProfiles() {
    ComPtr<ITfInputProcessorProfileMgr> manager;
    HRESULT hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&manager));
    if (FAILED(hr)) return hr;
    manager->UnregisterProfile(kClsid, kLanguage, kProfile, 0);
    ComPtr<ITfInputProcessorProfiles> profiles;
    if (SUCCEEDED(manager.As(&profiles))) profiles->Unregister(kClsid);
    return S_OK;
}

HRESULT RegisterCategories() {
    ComPtr<ITfCategoryMgr> categories;
    HRESULT hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&categories));
    if (FAILED(hr)) return hr;
    for (const GUID* category : kCategories) {
        hr = categories->RegisterCategory(kClsid, *category, kClsid);
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}

HRESULT UnregisterCategories() {
    ComPtr<ITfCategoryMgr> categories;
    HRESULT hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&categories));
    if (FAILED(hr)) return hr;
    for (const GUID* category : kCategories) categories->UnregisterCategory(kClsid, *category, kClsid);
    return S_OK;
}

}  // namespace bada::tip
