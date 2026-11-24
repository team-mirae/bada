// The TSF text input processor (an in-proc COM server): the DLL's exports and the class factory.
// Adapted from bada-windows src/tip/dllmain.cpp (jis-moonshot's).
#include <windows.h>

#include <new>

#include "candidate_window.h"
#include "dll.h"
#include "guids.h"
#include "log.h"
#include "shared_session.h"
#include "registration.h"
#include "text_service.h"

namespace bada::tip {
namespace {

HINSTANCE g_module = nullptr;
LONG g_refs = 0;

class ClassFactory final : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_INVALIDARG;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *object = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    // A static object: its references are lock counts on the DLL.
    STDMETHODIMP_(ULONG) AddRef() override {
        DllAddRef();
        return 2;
    }
    STDMETHODIMP_(ULONG) Release() override {
        DllRelease();
        return 1;
    }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** object) override {
        if (!object) return E_INVALIDARG;
        *object = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* service = new (std::nothrow) TextService();
        if (!service) return E_OUTOFMEMORY;
        const HRESULT hr = service->QueryInterface(riid, object);
        service->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        lock ? DllAddRef() : DllRelease();
        return S_OK;
    }
};

ClassFactory g_factory;

}  // namespace

HINSTANCE DllInstance() { return g_module; }
void DllAddRef() { InterlockedIncrement(&g_refs); }
void DllRelease() { InterlockedDecrement(&g_refs); }

}  // namespace bada::tip

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        bada::tip::g_module = instance;
        DisableThreadLibraryCalls(instance);
    } else if (reason == DLL_PROCESS_DETACH && !reserved) {
        bada::tip::CandidateWindow::Unregister();
    }
    return TRUE;
}

// The engine's session is freed here, outside the loader lock, once nothing references the DLL.
STDAPI DllCanUnloadNow() {
    if (bada::tip::g_refs) return S_FALSE;
    bada::tip::ReleaseProcess();
    return S_OK;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** object) {
    if (!object) return E_INVALIDARG;
    *object = nullptr;
    if (!IsEqualCLSID(clsid, bada::tip::kClsid)) return CLASS_E_CLASSNOTAVAILABLE;
    return bada::tip::g_factory.QueryInterface(riid, object);
}

STDAPI DllUnregisterServer() {
    bada::tip::UnregisterCategories();
    bada::tip::UnregisterProfiles();
    return bada::tip::UnregisterServer();
}

STDAPI DllRegisterServer() {
    HRESULT hr = bada::tip::RegisterServer(bada::tip::g_module);
    if (SUCCEEDED(hr)) hr = bada::tip::RegisterProfiles(bada::tip::g_module);
    if (SUCCEEDED(hr)) hr = bada::tip::RegisterCategories();
    if (FAILED(hr)) DllUnregisterServer();
    return hr;
}
