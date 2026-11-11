#include "WindowsEqApo.h"

#include "ApoLog.h"
#include "StatusShm.h"
#include "weq/Ids.h"

#include <combaseapi.h>

#include <cstring>
#include <cwchar>

namespace weq {

extern volatile LONG g_dllObjects; // Dll.cpp

// CRegAPOProperties<1> has a zero-length "additional IIDs" array by design.
#pragma warning(suppress : 4815)
const CRegAPOProperties<1> WindowsEqApo::sRegProperties(
    kClsidApo, kApoFriendlyName, kApoCopyright, kApoVersionMajor, kApoVersionMinor,
    __uuidof(IAudioProcessingObject),
    static_cast<APO_FLAG>(APO_FLAG_SAMPLESPERFRAME_MUST_MATCH | APO_FLAG_FRAMESPERSECOND_MUST_MATCH |
                          APO_FLAG_BITSPERSAMPLE_MUST_MATCH | APO_FLAG_INPLACE));

namespace {

void guidToString(const GUID& g, wchar_t (&out)[40])
{
    if (StringFromGUID2(g, out, 40) == 0)
        out[0] = L'\0';
}

} // namespace

WindowsEqApo::WindowsEqApo(IUnknown* outer)
    : CBaseAudioProcessingObject(sRegProperties)
    , outer_(outer ? outer : nonDelegatingUnknown())
{
    InterlockedIncrement(&g_dllObjects);
    apoLog(L"constructing (%s)", outer ? L"aggregated" : L"standalone");
    status_ = claimStatusSlot();
}

WindowsEqApo::~WindowsEqApo()
{
    apoLog(L"destroyed (endpoint %s)", endpointId_[0] ? endpointId_ : L"?");
    releaseStatusSlot(status_);
    InterlockedDecrement(&g_dllObjects);
}

// ------------------------------------------------------------------ IUnknown

HRESULT STDMETHODCALLTYPE WindowsEqApo::NonDelegatingQueryInterface(REFIID riid, void** ppv)
{
    if (!ppv)
        return E_POINTER;
    *ppv = nullptr;

    if (riid == __uuidof(IUnknown)) {
        // Identity: the inner (non-delegating) unknown, as aggregation requires.
        *ppv = nonDelegatingUnknown();
        NonDelegatingAddRef();
        return S_OK;
    }

    IUnknown* itf = nullptr;
    if (riid == __uuidof(IAudioProcessingObject))
        itf = static_cast<IAudioProcessingObject*>(this);
    else if (riid == __uuidof(IAudioProcessingObjectRT))
        itf = static_cast<IAudioProcessingObjectRT*>(this);
    else if (riid == __uuidof(IAudioProcessingObjectConfiguration))
        itf = static_cast<IAudioProcessingObjectConfiguration*>(this);
    else if (riid == __uuidof(IAudioSystemEffects))
        itf = static_cast<IAudioSystemEffects*>(this);
    else if (riid == __uuidof(IAudioSystemEffects2))
        itf = static_cast<IAudioSystemEffects2*>(this);

    if (!itf) {
        wchar_t iid[40];
        guidToString(riid, iid);
        apoLog(L"QueryInterface: not supported %s", iid);
        return E_NOINTERFACE;
    }
    itf->AddRef(); // goes through outer_, per aggregation rules
    *ppv = itf;
    return S_OK;
}

ULONG STDMETHODCALLTYPE WindowsEqApo::NonDelegatingAddRef()
{
    return static_cast<ULONG>(InterlockedIncrement(&refCount_));
}

ULONG STDMETHODCALLTYPE WindowsEqApo::NonDelegatingRelease()
{
    const LONG n = InterlockedDecrement(&refCount_);
    if (n == 0)
        delete this;
    return static_cast<ULONG>(n);
}

// ------------------------------------------------------------------ setup

STDMETHODIMP WindowsEqApo::Initialize(UINT32 cbDataSize, BYTE* pbyData)
{
    if (!pbyData && cbDataSize != 0)
        return E_POINTER;

    EnterCriticalSection(&m_CritSec);
    if (m_bIsInitialized) {
        LeaveCriticalSection(&m_CritSec);
        return APOERR_ALREADY_INITIALIZED;
    }

    GUID mode = GUID{};
    BOOL discoveryOnly = FALSE;
    IMMDeviceCollection* devices = nullptr;
    IPropertyStore* endpointProps = nullptr;
    UINT deviceIndex = 0;
    const wchar_t* kind = L"base";

    if (cbDataSize == sizeof(APOInitSystemEffects2)) {
        const auto* init = reinterpret_cast<const APOInitSystemEffects2*>(pbyData);
        mode = init->AudioProcessingMode;
        discoveryOnly = init->InitializeForDiscoveryOnly;
        devices = init->pDeviceCollection;
        endpointProps = init->pAPOEndpointProperties;
        deviceIndex = init->nSoftwareIoDeviceInCollection;
        kind = L"SystemEffects2";
    } else if (cbDataSize == sizeof(APOInitSystemEffects)) {
        const auto* init = reinterpret_cast<const APOInitSystemEffects*>(pbyData);
        devices = init->pDeviceCollection;
        endpointProps = init->pAPOEndpointProperties;
        kind = L"SystemEffects";
    } else if (cbDataSize < sizeof(APOInitBaseStruct)) {
        LeaveCriticalSection(&m_CritSec);
        return E_INVALIDARG;
    }

    // Endpoint identity: PKEY_AudioEndpoint_GUID from the endpoint's property
    // store gives the MMDevice id ("{0.0.0.00000000}.{guid}" for render).
    // The device collection's "software IO" item is the KS adapter instead,
    // which is only a fallback.
    if (endpointProps) {
        constexpr PROPERTYKEY kEndpointGuid = {
            {0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}}, 4};
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(endpointProps->GetValue(kEndpointGuid, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
            swprintf_s(endpointId_, L"{0.0.0.00000000}.%s", v.pwszVal);
        PropVariantClear(&v);
    }

    if (!endpointId_[0] && devices) {
        IMMDevice* device = nullptr;
        if (SUCCEEDED(devices->Item(deviceIndex, &device)) && device) {
            LPWSTR id = nullptr;
            if (SUCCEEDED(device->GetId(&id)) && id) {
                wcsncpy_s(endpointId_, id, _TRUNCATE);
                CoTaskMemFree(id);
            }
            device->Release();
        }
    }

    m_bIsInitialized = true;
    LeaveCriticalSection(&m_CritSec);

    if (status_) {
        status_->processingMode = mode;
        status_->discoveryOnly = discoveryOnly ? 1u : 0u;
        wcsncpy_s(status_->endpointId, endpointId_, _TRUNCATE);
    }

    wchar_t modeStr[40];
    guidToString(mode, modeStr);
    apoLog(L"initialized (%s) endpoint=%s mode=%s discoveryOnly=%d statusSlot=%s",
           kind, endpointId_[0] ? endpointId_ : L"?", modeStr, discoveryOnly ? 1 : 0,
           status_ ? L"yes" : L"no");
    return S_OK;
}

namespace {

void logFormatResult(const wchar_t* which, HRESULT hr, IAudioMediaType* requested)
{
    if (hr == S_OK)
        return;
    UNCOMPRESSEDAUDIOFORMAT f{};
    if (requested && SUCCEEDED(requested->GetUncompressedAudioFormat(&f))) {
        wchar_t type[40];
        guidToString(f.guidFormatType, type);
        apoLog(L"%s format not accepted as-is (hr=0x%08lX): %s %lu ch %lu-bit/%lu container @ %.0f Hz",
               which, static_cast<unsigned long>(hr), type, f.dwSamplesPerFrame, f.dwValidBitsPerSample,
               f.dwBytesPerSampleContainer * 8, static_cast<double>(f.fFramesPerSecond));
    } else {
        apoLog(L"%s format not accepted (hr=0x%08lX)", which, static_cast<unsigned long>(hr));
    }
}

} // namespace

STDMETHODIMP WindowsEqApo::IsInputFormatSupported(IAudioMediaType* pOppositeFormat, IAudioMediaType* pRequestedInputFormat,
                                                  IAudioMediaType** ppSupportedInputFormat)
{
    const HRESULT hr = CBaseAudioProcessingObject::IsInputFormatSupported(pOppositeFormat, pRequestedInputFormat,
                                                                          ppSupportedInputFormat);
    logFormatResult(L"input", hr, pRequestedInputFormat);
    return hr;
}

STDMETHODIMP WindowsEqApo::IsOutputFormatSupported(IAudioMediaType* pOppositeFormat, IAudioMediaType* pRequestedOutputFormat,
                                                   IAudioMediaType** ppSupportedOutputFormat)
{
    const HRESULT hr = CBaseAudioProcessingObject::IsOutputFormatSupported(pOppositeFormat, pRequestedOutputFormat,
                                                                           ppSupportedOutputFormat);
    logFormatResult(L"output", hr, pRequestedOutputFormat);
    return hr;
}

STDMETHODIMP WindowsEqApo::LockForProcess(UINT32 u32NumInputConnections, APO_CONNECTION_DESCRIPTOR** ppInputConnections,
                                          UINT32 u32NumOutputConnections, APO_CONNECTION_DESCRIPTOR** ppOutputConnections)
{
    EnterCriticalSection(&m_CritSec);
    const HRESULT hr = CBaseAudioProcessingObject::LockForProcess(
        u32NumInputConnections, ppInputConnections, u32NumOutputConnections, ppOutputConnections);
    if (SUCCEEDED(hr))
        channels_ = GetSamplesPerFrame();
    const float rate = SUCCEEDED(hr) ? GetFramesPerSecond() : 0.0f;
    LeaveCriticalSection(&m_CritSec);

    if (SUCCEEDED(hr) && status_) {
        status_->sampleRate = static_cast<uint32_t>(rate);
        status_->channels = channels_;
        status_->framesProcessed = 0;
        MemoryBarrier();
        status_->state = kInstanceLocked;
    }

    apoLog(L"LockForProcess hr=0x%08lX endpoint=%s format=%u ch @ %.0f Hz", static_cast<unsigned long>(hr),
           endpointId_[0] ? endpointId_ : L"?", channels_, static_cast<double>(rate));
    return hr;
}

STDMETHODIMP WindowsEqApo::UnlockForProcess()
{
    EnterCriticalSection(&m_CritSec);
    const HRESULT hr = CBaseAudioProcessingObject::UnlockForProcess();
    LeaveCriticalSection(&m_CritSec);

    if (status_)
        status_->state = kInstanceInitialized;
    apoLog(L"UnlockForProcess hr=0x%08lX endpoint=%s: %llu APOProcess calls, %llu frames (%llu with audio, %llu silent)",
           static_cast<unsigned long>(hr), endpointId_[0] ? endpointId_ : L"?",
           static_cast<unsigned long long>(processCalls_), static_cast<unsigned long long>(framesValid_ + framesSilent_),
           static_cast<unsigned long long>(framesValid_), static_cast<unsigned long long>(framesSilent_));
    processCalls_ = framesValid_ = framesSilent_ = 0;
    return hr;
}

// ------------------------------------------------------------------ real time

#pragma AVRT_CODE_BEGIN
STDMETHODIMP_(void) WindowsEqApo::APOProcess(UINT32 u32NumInputConnections, APO_CONNECTION_PROPERTY** ppInputConnections,
                                             UINT32 u32NumOutputConnections, APO_CONNECTION_PROPERTY** ppOutputConnections)
{
    if (u32NumInputConnections < 1 || u32NumOutputConnections < 1)
        return;

    APO_CONNECTION_PROPERTY* in = ppInputConnections[0];
    APO_CONNECTION_PROPERTY* out = ppOutputConnections[0];
    const UINT32 frames = in->u32ValidFrameCount;
    ++processCalls_;

    switch (in->u32BufferFlags) {
    case BUFFER_VALID:
        if (in->pBuffer != out->pBuffer)
            std::memcpy(reinterpret_cast<void*>(out->pBuffer), reinterpret_cast<const void*>(in->pBuffer),
                        static_cast<size_t>(frames) * channels_ * sizeof(float));
        out->u32ValidFrameCount = frames;
        out->u32BufferFlags = BUFFER_VALID;
        framesValid_ += frames;
        break;

    case BUFFER_SILENT:
        out->u32ValidFrameCount = frames;
        out->u32BufferFlags = BUFFER_SILENT;
        framesSilent_ += frames;
        break;

    default: // BUFFER_INVALID: nothing to process
        out->u32ValidFrameCount = frames;
        out->u32BufferFlags = in->u32BufferFlags;
        break;
    }

    if (status_) {
        status_->framesProcessed += frames;   // single writer (this RT thread)
        status_->lastProcessTick = GetTickCount64();
    }
}
#pragma AVRT_CODE_END

// ------------------------------------------------------------------ IAudioSystemEffects2

STDMETHODIMP WindowsEqApo::GetEffectsList(LPGUID* ppEffectsIds, UINT* pcEffects, HANDLE /*Event*/)
{
    if (!ppEffectsIds || !pcEffects)
        return E_POINTER;
    *ppEffectsIds = nullptr; // no individually switchable effects (yet)
    *pcEffects = 0;
    return S_OK;
}

} // namespace weq
