#include <initguid.h> // instantiate the PROPERTYKEYs used below (this TU only)

#include "Devices.h"

#include "Util.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>

#include <algorithm>

namespace weq {

namespace {

constexpr wchar_t kFxGuid[] = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d}";
constexpr wchar_t kModesGuid[] = L"{d3993a3f-99c2-4402-b5ec-a92a0367664b}";

template <typename T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

std::wstring readStringValue(HKEY key, const std::wstring& name)
{
    wchar_t buf[512];
    DWORD size = sizeof(buf), type = 0;
    if (RegQueryValueExW(key, name.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(buf), &size) != ERROR_SUCCESS)
        return {};
    if (type != REG_SZ && type != REG_EXPAND_SZ)
        return {};
    return std::wstring(buf, wcsnlen(buf, size / sizeof(wchar_t)));
}

std::wstring propString(IPropertyStore* store, const PROPERTYKEY& key)
{
    PROPVARIANT v;
    PropVariantInit(&v);
    std::wstring out;
    if (SUCCEEDED(store->GetValue(key, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
        out = v.pwszVal;
    PropVariantClear(&v);
    return out;
}

} // namespace

const wchar_t* slotName(FxSlot slot)
{
    switch (slot) {
    case FxSlot::SFX: return L"SFX";
    case FxSlot::MFX: return L"MFX";
    case FxSlot::EFX: return L"EFX";
    }
    return L"?";
}

bool parseSlot(const std::wstring& s, FxSlot& out)
{
    if (iequals(s, L"sfx")) { out = FxSlot::SFX; return true; }
    if (iequals(s, L"mfx")) { out = FxSlot::MFX; return true; }
    if (iequals(s, L"efx")) { out = FxSlot::EFX; return true; }
    return false;
}

bool parseSlots(const std::wstring& s, std::vector<FxSlot>& out)
{
    out.clear();
    size_t start = 0;
    while (start <= s.size()) {
        const size_t comma = s.find(L',', start);
        const std::wstring part = s.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start);
        FxSlot slot;
        if (!parseSlot(part, slot))
            return false;
        bool dup = false;
        for (FxSlot x : out)
            dup = dup || x == slot;
        if (!dup)
            out.push_back(slot);
        if (comma == std::wstring::npos)
            break;
        start = comma + 1;
    }
    std::sort(out.begin(), out.end());
    return !out.empty();
}

std::wstring slotsName(const std::vector<FxSlot>& slots)
{
    std::wstring s;
    for (FxSlot x : slots) {
        if (!s.empty())
            s += L",";
        s += slotName(x);
    }
    return s;
}

std::wstring fxClsidValueName(FxSlot slot)
{
    const wchar_t* pid = slot == FxSlot::SFX ? L",5" : slot == FxSlot::MFX ? L",6" : L",7";
    return std::wstring(kFxGuid) + pid;
}

std::wstring fxModesValueName(FxSlot slot)
{
    const wchar_t* pid = slot == FxSlot::SFX ? L",5" : slot == FxSlot::MFX ? L",6" : L",7";
    return std::wstring(kModesGuid) + pid;
}

std::wstring fxAssociationValueName()
{
    return std::wstring(kFxGuid) + L",0";
}

std::wstring renderKeyPath(const std::wstring& guid)
{
    return L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\" + guid;
}

std::vector<RenderDevice> listRenderDevices(std::wstring& error)
{
    std::vector<RenderDevice> out;

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr)) {
        error = L"create device enumerator: " + hrText(hr);
        return out;
    }

    std::wstring defaultId;
    {
        ComPtr<IMMDevice> def;
        LPWSTR id = nullptr;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &def)) &&
            SUCCEEDED(def->GetId(&id))) {
            defaultId = id;
            CoTaskMemFree(id);
        }
    }

    ComPtr<IMMDeviceCollection> collection;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) {
        error = L"enumerate endpoints: " + hrText(hr);
        return out;
    }
    UINT count = 0;
    collection->GetCount(&count);

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(collection->Item(i, &dev)))
            continue;

        RenderDevice d;
        d.index = static_cast<int>(out.size());
        LPWSTR id = nullptr;
        if (FAILED(dev->GetId(&id)))
            continue;
        d.id = id;
        CoTaskMemFree(id);
        d.isDefault = d.id == defaultId;
        const size_t dot = d.id.rfind(L'.');
        d.guid = dot == std::wstring::npos ? d.id : d.id.substr(dot + 1);

        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
            d.name = propString(props.p, PKEY_Device_DeviceDesc);
            d.adapter = propString(props.p, PKEY_DeviceInterface_FriendlyName);
            if (d.name.empty())
                d.name = propString(props.p, PKEY_Device_FriendlyName);

            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(props->GetValue(PKEY_AudioEndpoint_Disable_SysFx, &v)) && v.vt == VT_UI4)
                d.sysFxDisabled = v.ulVal != 0;
            PropVariantClear(&v);
        }

        HKEY fx = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, (renderKeyPath(d.guid) + L"\\FxProperties").c_str(), 0,
                          KEY_READ, &fx) == ERROR_SUCCESS) {
            d.hasFxKey = true;
            d.sfx = readStringValue(fx, fxClsidValueName(FxSlot::SFX));
            d.mfx = readStringValue(fx, fxClsidValueName(FxSlot::MFX));
            d.efx = readStringValue(fx, fxClsidValueName(FxSlot::EFX));
            d.lfx = readStringValue(fx, std::wstring(kFxGuid) + L",1");
            d.gfx = readStringValue(fx, std::wstring(kFxGuid) + L",2");
            RegCloseKey(fx);
        }

        out.push_back(std::move(d));
    }
    return out;
}

const RenderDevice* selectDevice(const std::vector<RenderDevice>& devices, const std::wstring& selector,
                                 std::wstring& error)
{
    if (devices.empty()) {
        error = L"no active playback devices";
        return nullptr;
    }
    if (selector.empty()) {
        for (const RenderDevice& d : devices)
            if (d.isDefault)
                return &d;
        error = L"no default playback device; pass --device";
        return nullptr;
    }

    // Index
    if (selector.find_first_not_of(L"0123456789") == std::wstring::npos) {
        const int idx = _wtoi(selector.c_str());
        if (idx >= 0 && idx < static_cast<int>(devices.size()))
            return &devices[static_cast<size_t>(idx)];
        error = L"device index out of range";
        return nullptr;
    }

    // GUID or full id
    for (const RenderDevice& d : devices)
        if (iequals(d.guid, selector) || iequals(d.id, selector) || iequals(d.guid, L"{" + selector + L"}"))
            return &d;

    // Name substring (must be unique)
    const RenderDevice* match = nullptr;
    for (const RenderDevice& d : devices) {
        if (icontains(d.displayName(), selector)) {
            if (match) {
                error = L"\"" + selector + L"\" matches more than one device; use the index or GUID";
                return nullptr;
            }
            match = &d;
        }
    }
    if (!match)
        error = L"no device matches \"" + selector + L"\"";
    return match;
}

} // namespace weq
