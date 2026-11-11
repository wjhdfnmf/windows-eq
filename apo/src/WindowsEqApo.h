#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioenginebaseapo.h>
#include <BaseAudioProcessingObject.h>

#include "weq/Status.h"

namespace weq {

// The "inner" IUnknown of an aggregatable COM object. Same vtable shape as
// IUnknown, so a pointer to it can be handed out as the object's identity.
struct INonDelegatingUnknown {
    virtual HRESULT STDMETHODCALLTYPE NonDelegatingQueryInterface(REFIID riid, void** ppv) = 0;
    virtual ULONG STDMETHODCALLTYPE NonDelegatingAddRef() = 0;
    virtual ULONG STDMETHODCALLTYPE NonDelegatingRelease() = 0;
};

// System-effects APO. Phase 2: bit-exact passthrough that reports its state
// (endpoint, format, frames processed) through the shared status block.
//
// The audio engine creates APOs aggregated (CreateInstance with an outer
// IUnknown), so every IUnknown call on our interfaces delegates to outer_.
// Without an outer object, outer_ is our own non-delegating unknown.
class WindowsEqApo final : public CBaseAudioProcessingObject, public IAudioSystemEffects2, public INonDelegatingUnknown {
public:
    static const CRegAPOProperties<1> sRegProperties;

    explicit WindowsEqApo(IUnknown* outer);
    ~WindowsEqApo() override;

    IUnknown* nonDelegatingUnknown()
    {
        return reinterpret_cast<IUnknown*>(static_cast<INonDelegatingUnknown*>(this));
    }

    // INonDelegatingUnknown
    HRESULT STDMETHODCALLTYPE NonDelegatingQueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE NonDelegatingAddRef() override;
    ULONG STDMETHODCALLTYPE NonDelegatingRelease() override;

    // IUnknown (delegating)
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override { return outer_->QueryInterface(riid, ppv); }
    STDMETHOD_(ULONG, AddRef)() override { return outer_->AddRef(); }
    STDMETHOD_(ULONG, Release)() override { return outer_->Release(); }

    // IAudioProcessingObject
    STDMETHOD(Initialize)(UINT32 cbDataSize, BYTE* pbyData) override;
    STDMETHOD(IsInputFormatSupported)(IAudioMediaType* pOppositeFormat, IAudioMediaType* pRequestedInputFormat,
                                      IAudioMediaType** ppSupportedInputFormat) override;
    STDMETHOD(IsOutputFormatSupported)(IAudioMediaType* pOppositeFormat, IAudioMediaType* pRequestedOutputFormat,
                                       IAudioMediaType** ppSupportedOutputFormat) override;

    // IAudioProcessingObjectConfiguration
    STDMETHOD(LockForProcess)(UINT32 u32NumInputConnections, APO_CONNECTION_DESCRIPTOR** ppInputConnections,
                              UINT32 u32NumOutputConnections, APO_CONNECTION_DESCRIPTOR** ppOutputConnections) override;
    STDMETHOD(UnlockForProcess)() override;

    // IAudioProcessingObjectRT
    STDMETHOD_(void, APOProcess)(UINT32 u32NumInputConnections, APO_CONNECTION_PROPERTY** ppInputConnections,
                                 UINT32 u32NumOutputConnections, APO_CONNECTION_PROPERTY** ppOutputConnections) override;

    // IAudioSystemEffects2
    STDMETHOD(GetEffectsList)(LPGUID* ppEffectsIds, UINT* pcEffects, HANDLE Event) override;

private:
    IUnknown* outer_;          // not AddRef'd (COM aggregation rules)
    volatile LONG refCount_ = 1;
    InstanceStatus* status_ = nullptr;
    UINT32 channels_ = 0;
    // Written by the real-time thread only; read after UnlockForProcess.
    uint64_t processCalls_ = 0, framesValid_ = 0, framesSilent_ = 0;
    wchar_t endpointId_[96] = {};
};

} // namespace weq
