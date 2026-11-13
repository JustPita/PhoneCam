#include "vcamsource.h"

#include <cstdarg>
#include <cstdio>
#include <string>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")

std::atomic<long> g_objectCount{0};
static std::atomic<long> g_lockCount{0};
static HMODULE g_module = nullptr;

// Journal de débogage : visible avec DebugView (OutputDebugString), y compris depuis le service.
void Log(const wchar_t* fmt, ...) {
    wchar_t buf[512];
    int n = swprintf_s(buf, L"[PhoneCamSource %lu] ", GetCurrentProcessId());
    va_list ap;
    va_start(ap, fmt);
    vswprintf_s(buf + n, _countof(buf) - n, fmt, ap);
    va_end(ap);
    wcscat_s(buf, L"\n");
    OutputDebugStringW(buf);
}

// --- Activator ----------------------------------------------------------------
Activator::Activator() { g_objectCount++; }

ULONG Activator::Release() {
    LONG r = InterlockedDecrement(&m_ref);
    if (r == 0) delete this;
    return r;
}

STDMETHODIMP Activator::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFAttributes) || riid == __uuidof(IMFActivate)) {
        *ppv = static_cast<IMFActivate*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP Activator::ActivateObject(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (!m_source) {
        auto* src = new (std::nothrow) MediaSource();
        if (!src) return E_OUTOFMEMORY;
        HRESULT hr = src->Initialize();
        if (FAILED(hr)) {
            Log(L"MediaSource::Initialize a échoué : 0x%08X", hr);
            src->Shutdown();
            src->Release();
            return hr;
        }
        m_source.Attach(static_cast<IMFMediaSourceEx*>(src));
    }
    return m_source->QueryInterface(riid, ppv);
}

STDMETHODIMP Activator::ShutdownObject() {
    if (m_source) m_source->Shutdown();
    m_source.Reset();
    return S_OK;
}

STDMETHODIMP Activator::DetachObject() {
    m_source.Reset();
    return S_OK;
}

// --- Fabrique de classe -------------------------------------------------------
class ClassFactory final : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* a = new (std::nothrow) Activator();
        if (!a) return E_OUTOFMEMORY;
        HRESULT hr = a->Initialize();
        if (SUCCEEDED(hr)) hr = a->QueryInterface(riid, ppv);
        a->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        lock ? g_lockCount++ : g_lockCount--;
        return S_OK;
    }
};
static ClassFactory g_factory;

// --- Exports COM --------------------------------------------------------------
STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (clsid != CLSID_PhoneCamSource) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow() { return (g_objectCount == 0 && g_lockCount == 0) ? S_OK : S_FALSE; }

static LSTATUS SetKey(HKEY root, const std::wstring& path, const wchar_t* name, const std::wstring& value) {
    HKEY key = nullptr;
    LSTATUS s = RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr);
    if (s != ERROR_SUCCESS) return s;
    s = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                       DWORD((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return s;
}

// Enregistrement dans HKLM : le service Frame Server (LocalService) ne voit pas le HKCU de l'utilisateur.
STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_module, path, MAX_PATH);
    const std::wstring clsidKey = std::wstring(L"Software\\Classes\\CLSID\\") + PHONECAM_CLSID_STRING;
    LSTATUS s = SetKey(HKEY_LOCAL_MACHINE, clsidKey, nullptr, L"PhoneCam Source");
    if (s == ERROR_SUCCESS) s = SetKey(HKEY_LOCAL_MACHINE, clsidKey + L"\\InprocServer32", nullptr, path);
    if (s == ERROR_SUCCESS) s = SetKey(HKEY_LOCAL_MACHINE, clsidKey + L"\\InprocServer32", L"ThreadingModel", L"Both");
    return HRESULT_FROM_WIN32(s);
}

STDAPI DllUnregisterServer() {
    const std::wstring clsidKey = std::wstring(L"Software\\Classes\\CLSID\\") + PHONECAM_CLSID_STRING;
    LSTATUS s = RegDeleteTreeW(HKEY_LOCAL_MACHINE, clsidKey.c_str());
    return (s == ERROR_SUCCESS || s == ERROR_FILE_NOT_FOUND) ? S_OK : HRESULT_FROM_WIN32(s);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
