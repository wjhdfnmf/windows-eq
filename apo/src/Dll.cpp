// COM in-process server plumbing for the APO: class factory, DllGetClassObject,
// DllCanUnloadNow and self-registration (COM class + APO registration).

#include "WindowsEqApo.h"

#include "ApoLog.h"
#include "weq/Ids.h"

#include <new>
#include <string>

namespace weq {

volatile LONG g_dllObjects = 0;
volatile LONG g_dllLocks = 0;
HMODULE g_module = nullptr;

namespace {

class ClassFactory final : public IClassFactory {
public:
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        apoLog(L"ClassFactory::QueryInterface: unsupported interface");
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return 2; }  // static lifetime
    STDMETHOD_(ULONG, Release)() override { return 1; }

    STDMETHOD(CreateInstance)(IUnknown* outer, REFIID riid, void** ppv) override
    {
        if (!ppv)
            return E_POINTER;
        *ppv = nullptr;

        wchar_t iid[40];
        if (StringFromGUID2(riid, iid, 40) == 0)
            iid[0] = L'\0';

        // COM rule: an aggregated object may only be asked for IUnknown.
        // The Windows audio engine aggregates APOs, so this path is the norm.
        if (outer && riid != __uuidof(IUnknown)) {
            apoLog(L"CreateInstance: aggregated request for %s rejected", iid);
            return CLASS_E_NOAGGREGATION;
        }

        auto* apo = new (std::nothrow) WindowsEqApo(outer);
        if (!apo)
            return E_OUTOFMEMORY;
        // Go through the inner unknown so the reference lands on our object.
        const HRESULT hr = apo->NonDelegatingQueryInterface(riid, ppv);
        apo->NonDelegatingRelease(); // drop the construction reference
        apoLog(L"CreateInstance(%s, %s) hr=0x%08lX", outer ? L"aggregated" : L"standalone", iid,
               static_cast<unsigned long>(hr));
        return hr;
    }

    STDMETHOD(LockServer)(BOOL lock) override
    {
        if (lock)
            InterlockedIncrement(&g_dllLocks);
        else
            InterlockedDecrement(&g_dllLocks);
        return S_OK;
    }
};

ClassFactory g_factory;

LSTATUS setString(HKEY key, const wchar_t* name, const wchar_t* value)
{
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value),
                          static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
}

std::wstring clsidKeyPath()
{
    return std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kClsidApoString;
}

} // namespace

} // namespace weq

using namespace weq;

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
        // Diagnostic: proves the DLL was loaded even if COM creation never
        // happens. Only kernel32 calls (file I/O), which is safe in DllMain.
        wchar_t exe[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        apoLog(L"DLL loaded into %s (v%u.%u)", n ? exe : L"?", kApoVersionMajor, kApoVersionMinor);
    } else if (reason == DLL_PROCESS_DETACH) {
        apoLog(L"DLL unloading");
    }
    return TRUE;
}

_Check_return_ STDAPI DllGetClassObject(_In_ REFCLSID rclsid, _In_ REFIID riid, _Outptr_ LPVOID* ppv)
{
    if (!ppv)
        return E_POINTER;
    *ppv = nullptr;
    if (rclsid != kClsidApo) {
        apoLog(L"DllGetClassObject: unknown CLSID");
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    return g_factory.QueryInterface(riid, ppv);
}

__control_entrypoint(DllExport) STDAPI DllCanUnloadNow()
{
    return (g_dllObjects == 0 && g_dllLocks == 0) ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer()
{
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(g_module, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return HRESULT_FROM_WIN32(GetLastError());

    // COM class under HKLM (audiodg runs as LOCAL SERVICE, so HKCU would be invisible).
    HKEY clsidKey = nullptr, inproc = nullptr;
    LSTATUS st = RegCreateKeyExW(HKEY_LOCAL_MACHINE, clsidKeyPath().c_str(), 0, nullptr, 0,
                                 KEY_WRITE, nullptr, &clsidKey, nullptr);
    if (st != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(st);
    setString(clsidKey, nullptr, L"Windows EQ Audio Processing Object");
    st = RegCreateKeyExW(clsidKey, L"InprocServer32", 0, nullptr, 0, KEY_WRITE, nullptr, &inproc, nullptr);
    if (st == ERROR_SUCCESS) {
        setString(inproc, nullptr, path);
        setString(inproc, L"ThreadingModel", L"Both");
        RegCloseKey(inproc);
    }
    RegCloseKey(clsidKey);
    if (st != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(st);

    // APO registration: HKLM\SOFTWARE\Classes\AudioEngine\AudioProcessingObjects\{clsid}
    const HRESULT hr = RegisterAPO(WindowsEqApo::sRegProperties);
    if (FAILED(hr)) {
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, clsidKeyPath().c_str());
        return hr;
    }
    return S_OK;
}

STDAPI DllUnregisterServer()
{
    const HRESULT hr = UnregisterAPO(kClsidApo);
    const LSTATUS st = RegDeleteTreeW(HKEY_LOCAL_MACHINE, clsidKeyPath().c_str());
    if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
        return hr;
    if (st != ERROR_SUCCESS && st != ERROR_FILE_NOT_FOUND)
        return HRESULT_FROM_WIN32(st);
    return S_OK;
}
