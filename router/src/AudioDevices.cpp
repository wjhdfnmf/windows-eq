#include <initguid.h>

#include "AudioDevices.h"

#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>

#include <algorithm>
#include <cwctype>

namespace weq {

namespace {

std::wstring lower(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

IMMDeviceEnumerator* enumerator(std::wstring& error)
{
    IMMDeviceEnumerator* e = nullptr;
    const HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&e));
    if (FAILED(hr))
        error = L"create device enumerator: " + hrString(hr);
    return e;
}

} // namespace

std::wstring hrString(HRESULT hr)
{
    wchar_t buf[32];
    swprintf_s(buf, L"0x%08lX", static_cast<unsigned long>(hr));
    wchar_t* msg = nullptr;
    std::wstring s = buf;
    if (FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr) && msg) {
        std::wstring m = msg;
        LocalFree(msg);
        while (!m.empty() && (m.back() == L'\n' || m.back() == L'\r' || m.back() == L'.'))
            m.pop_back();
        s = m + L" (" + s + L")";
    }
    return s;
}

std::vector<AudioDevice> listDevices(bool render, std::wstring& error)
{
    std::vector<AudioDevice> out;
    IMMDeviceEnumerator* e = enumerator(error);
    if (!e)
        return out;

    std::wstring defaultId;
    IMMDevice* def = nullptr;
    if (SUCCEEDED(e->GetDefaultAudioEndpoint(render ? eRender : eCapture, eConsole, &def))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(def->GetId(&id))) {
            defaultId = id;
            CoTaskMemFree(id);
        }
        def->Release();
    }

    IMMDeviceCollection* col = nullptr;
    if (SUCCEEDED(e->EnumAudioEndpoints(render ? eRender : eCapture, DEVICE_STATE_ACTIVE, &col))) {
        UINT n = 0;
        col->GetCount(&n);
        for (UINT i = 0; i < n; ++i) {
            IMMDevice* d = nullptr;
            if (FAILED(col->Item(i, &d)))
                continue;
            AudioDevice ad;
            ad.isRender = render;
            LPWSTR id = nullptr;
            if (SUCCEEDED(d->GetId(&id))) {
                ad.id = id;
                CoTaskMemFree(id);
            }
            IPropertyStore* ps = nullptr;
            if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR)
                    ad.name = v.pwszVal;
                PropVariantClear(&v);
                ps->Release();
            }
            d->Release();
            ad.isDefault = ad.id == defaultId;
            ad.index = static_cast<int>(out.size());
            out.push_back(std::move(ad));
        }
        col->Release();
    }
    e->Release();
    return out;
}

const AudioDevice* findDevice(const std::vector<AudioDevice>& devices, const std::wstring& selector, std::wstring& error)
{
    if (selector.empty()) {
        error = L"no device given";
        return nullptr;
    }
    if (selector.find_first_not_of(L"0123456789") == std::wstring::npos) {
        const int i = _wtoi(selector.c_str());
        if (i >= 0 && i < static_cast<int>(devices.size()))
            return &devices[static_cast<size_t>(i)];
        error = L"device index " + selector + L" out of range";
        return nullptr;
    }
    for (const AudioDevice& d : devices)
        if (lower(d.id) == lower(selector))
            return &d;
    const AudioDevice* match = nullptr;
    for (const AudioDevice& d : devices) {
        if (lower(d.name).find(lower(selector)) != std::wstring::npos) {
            if (match) {
                error = L"\"" + selector + L"\" matches more than one device; use the index";
                return nullptr;
            }
            match = &d;
        }
    }
    if (!match)
        error = L"no device matches \"" + selector + L"\"";
    return match;
}

IMMDevice* openDevice(const std::wstring& id, std::wstring& error)
{
    IMMDeviceEnumerator* e = enumerator(error);
    if (!e)
        return nullptr;
    IMMDevice* d = nullptr;
    const HRESULT hr = e->GetDevice(id.c_str(), &d);
    e->Release();
    if (FAILED(hr)) {
        error = L"open device: " + hrString(hr);
        return nullptr;
    }
    return d;
}

} // namespace weq
