// Loads WindowsEqApo.dll in-process and drives it like the audio engine does,
// without registering or installing anything.

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioenginebaseapo.h>
#include <audiomediatype.h>
#include <mmreg.h>

#include "weq/Ids.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#ifndef WEQ_APO_DLL
#error WEQ_APO_DLL must be defined
#endif

namespace {

int g_failures = 0;

#define CHECK(cond) \
    do { if (!(cond)) { std::printf("  FAIL line %d: %s\n", __LINE__, #cond); ++g_failures; } } while (0)
#define CHECK_HR(expr) \
    do { const HRESULT hr_ = (expr); if (FAILED(hr_)) { std::printf("  FAIL line %d: %s -> 0x%08lX\n", __LINE__, #expr, static_cast<unsigned long>(hr_)); ++g_failures; } } while (0)

constexpr GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr GUID kModeDefault = {0xc18e2f7e, 0x933d, 0x4965, {0xb7, 0xd1, 0x1e, 0xef, 0x22, 0x8d, 0x2a, 0xf3}};

IAudioMediaType* makeFormat(const GUID& subtype, WORD bits, WORD channels, DWORD rate)
{
    WAVEFORMATEXTENSIBLE wf{};
    wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wf.Format.nChannels = channels;
    wf.Format.nSamplesPerSec = rate;
    wf.Format.wBitsPerSample = bits;
    wf.Format.nBlockAlign = static_cast<WORD>(channels * bits / 8);
    wf.Format.nAvgBytesPerSec = rate * wf.Format.nBlockAlign;
    wf.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wf.Samples.wValidBitsPerSample = bits;
    wf.dwChannelMask = channels == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
    wf.SubFormat = subtype;
    IAudioMediaType* mt = nullptr;
    if (FAILED(CreateAudioMediaType(&wf.Format, sizeof(wf), &mt)))
        return nullptr;
    return mt;
}

template <typename T>
T* qi(IUnknown* u)
{
    T* p = nullptr;
    if (FAILED(u->QueryInterface(__uuidof(T), reinterpret_cast<void**>(&p))))
        return nullptr;
    return p;
}

// Minimal aggregating outer object, like the one the audio engine wraps APOs in.
struct Outer final : IUnknown {
    volatile LONG ref = 1;
    IUnknown* inner = nullptr; // the APO's non-delegating unknown (owned)

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override
    {
        if (riid == __uuidof(IUnknown)) {
            *ppv = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        return inner ? inner->QueryInterface(riid, ppv) : E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return static_cast<ULONG>(InterlockedIncrement(&ref)); }
    STDMETHOD_(ULONG, Release)() override { return static_cast<ULONG>(InterlockedDecrement(&ref)); }
};

void runFormat(IClassFactory* factory, WORD channels, DWORD rate, bool inPlace, bool aggregated)
{
    std::printf("format %u ch @ %lu Hz, %s, %s\n", channels, rate, inPlace ? "in-place" : "separate buffers",
                aggregated ? "aggregated" : "standalone");

    Outer outer;
    IAudioProcessingObject* apo = nullptr;
    if (aggregated) {
        // Aggregated creation may only ask for IUnknown.
        void* bad = nullptr;
        CHECK(factory->CreateInstance(&outer, __uuidof(IAudioProcessingObject), &bad) == CLASS_E_NOAGGREGATION);
        CHECK(bad == nullptr);
        CHECK_HR(factory->CreateInstance(&outer, __uuidof(IUnknown), reinterpret_cast<void**>(&outer.inner)));
        if (!outer.inner)
            return;
        CHECK_HR(outer.QueryInterface(__uuidof(IAudioProcessingObject), reinterpret_cast<void**>(&apo)));
        CHECK(outer.ref == 2); // the interface reference was counted on the outer object

        // Identity: IUnknown from any inner interface is the outer object.
        IUnknown* id = nullptr;
        CHECK_HR(apo->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&id)));
        CHECK(id == static_cast<IUnknown*>(&outer));
        if (id)
            id->Release();
    } else {
        CHECK_HR(factory->CreateInstance(nullptr, __uuidof(IAudioProcessingObject), reinterpret_cast<void**>(&apo)));
    }
    if (!apo)
        return;

    auto* rt = qi<IAudioProcessingObjectRT>(apo);
    auto* cfg = qi<IAudioProcessingObjectConfiguration>(apo);
    auto* fx2 = qi<IAudioSystemEffects2>(apo);
    CHECK(rt && cfg && fx2);
    if (auto* fx1 = qi<IAudioSystemEffects>(apo))
        fx1->Release();
    else
        CHECK(!"IAudioSystemEffects");
    IUnknown* bogus = nullptr;
    CHECK(apo->QueryInterface(IID_IClassFactory, reinterpret_cast<void**>(&bogus)) == E_NOINTERFACE);

    // Registration properties
    APO_REG_PROPERTIES* reg = nullptr;
    CHECK_HR(apo->GetRegistrationProperties(&reg));
    if (reg) {
        CHECK(reg->clsid == weq::kClsidApo);
        CHECK((reg->Flags & APO_FLAG_INPLACE) != 0);
        CHECK(reg->u32NumAPOInterfaces == 1);
        CoTaskMemFree(reg);
    }

    // Lock before Initialize must fail
    // Initialize as the audio engine does for system effects on Windows 8.1+
    APOInitSystemEffects2 init{};
    init.APOInit.cbSize = sizeof(init);
    init.APOInit.clsid = weq::kClsidApo;
    init.AudioProcessingMode = kModeDefault;
    CHECK_HR(apo->Initialize(sizeof(init), reinterpret_cast<BYTE*>(&init)));
    CHECK(apo->Initialize(sizeof(init), reinterpret_cast<BYTE*>(&init)) == APOERR_ALREADY_INITIALIZED);

    // Format negotiation: float is accepted, 16-bit PCM is not.
    IAudioMediaType* fmt = makeFormat(kSubtypeFloat, 32, channels, rate);
    IAudioMediaType* pcm = makeFormat(kSubtypePcm, 16, channels, rate);
    CHECK(fmt && pcm);
    IAudioMediaType* supported = nullptr;
    CHECK(apo->IsInputFormatSupported(fmt, fmt, &supported) == S_OK);
    if (supported) { supported->Release(); supported = nullptr; }
    CHECK(apo->IsOutputFormatSupported(fmt, fmt, &supported) == S_OK);
    if (supported) { supported->Release(); supported = nullptr; }
    const HRESULT pcmHr = apo->IsInputFormatSupported(fmt, pcm, &supported);
    CHECK(pcmHr != S_OK);
    if (supported) { supported->Release(); supported = nullptr; }

    HNSTIME latency = -1;
    CHECK_HR(apo->GetLatency(&latency));
    CHECK(latency == 0);

    // Lock
    const UINT32 maxFrames = rate / 100; // 10 ms
    std::vector<float> inBuf(static_cast<size_t>(maxFrames) * channels), outBuf(inBuf.size());
    float* outPtr = inPlace ? inBuf.data() : outBuf.data();

    APO_CONNECTION_DESCRIPTOR inDesc{APO_CONNECTION_BUFFER_TYPE_EXTERNAL, reinterpret_cast<UINT_PTR>(inBuf.data()),
                                     maxFrames, fmt, APO_CONNECTION_DESCRIPTOR_SIGNATURE};
    APO_CONNECTION_DESCRIPTOR outDesc{APO_CONNECTION_BUFFER_TYPE_EXTERNAL, reinterpret_cast<UINT_PTR>(outPtr),
                                      maxFrames, fmt, APO_CONNECTION_DESCRIPTOR_SIGNATURE};
    APO_CONNECTION_DESCRIPTOR* ins[] = {&inDesc};
    APO_CONNECTION_DESCRIPTOR* outs[] = {&outDesc};
    CHECK_HR(cfg->LockForProcess(1, ins, 1, outs));

    // Process: valid buffers must pass through bit-exact.
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    APO_CONNECTION_PROPERTY inProp{reinterpret_cast<UINT_PTR>(inBuf.data()), 0, BUFFER_VALID,
                                   APO_CONNECTION_PROPERTY_SIGNATURE};
    APO_CONNECTION_PROPERTY outProp{reinterpret_cast<UINT_PTR>(outPtr), 0, BUFFER_INVALID,
                                    APO_CONNECTION_PROPERTY_SIGNATURE};
    APO_CONNECTION_PROPERTY* inProps[] = {&inProp};
    APO_CONNECTION_PROPERTY* outProps[] = {&outProp};

    bool exact = true;
    for (int iter = 0; iter < 200; ++iter) {
        const UINT32 frames = maxFrames - (iter % 7);
        for (UINT32 i = 0; i < frames * channels; ++i)
            inBuf[i] = dist(rng);
        const std::vector<float> expected(inBuf.begin(), inBuf.begin() + frames * channels);
        inProp.u32ValidFrameCount = frames;
        inProp.u32BufferFlags = BUFFER_VALID;
        rt->APOProcess(1, inProps, 1, outProps);
        exact = exact && outProp.u32ValidFrameCount == frames && outProp.u32BufferFlags == BUFFER_VALID &&
                std::memcmp(outPtr, expected.data(), expected.size() * sizeof(float)) == 0;
    }
    CHECK(exact);

    // Silent buffers propagate as silent.
    inProp.u32BufferFlags = BUFFER_SILENT;
    inProp.u32ValidFrameCount = maxFrames;
    rt->APOProcess(1, inProps, 1, outProps);
    CHECK(outProp.u32BufferFlags == BUFFER_SILENT);
    CHECK(outProp.u32ValidFrameCount == maxFrames);

    CHECK(rt->CalcInputFrames(480) == 480);
    CHECK(rt->CalcOutputFrames(480) == 480);

    // Effects list
    LPGUID ids = reinterpret_cast<LPGUID>(1);
    UINT count = 99;
    CHECK_HR(fx2->GetEffectsList(&ids, &count, nullptr));
    CHECK(ids == nullptr && count == 0);

    CHECK_HR(cfg->UnlockForProcess());
    CHECK(cfg->UnlockForProcess() != S_OK); // already unlocked

    fmt->Release();
    pcm->Release();
    fx2->Release();
    cfg->Release();
    rt->Release();
    apo->Release();

    if (aggregated) {
        CHECK(outer.ref == 1); // every inner-interface reference went back to the outer
        outer.inner->Release(); // outer releases the inner object -> destroyed
        outer.inner = nullptr;
    }
}

} // namespace

int main()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    HMODULE dll = LoadLibraryW(WEQ_APO_DLL);
    if (!dll) {
        std::printf("FAIL: could not load %ls (%lu)\n", WEQ_APO_DLL, GetLastError());
        return 1;
    }
    using GetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);
    using CanUnloadFn = HRESULT(STDAPICALLTYPE*)();
    auto getClassObject = reinterpret_cast<GetClassObjectFn>(GetProcAddress(dll, "DllGetClassObject"));
    auto canUnload = reinterpret_cast<CanUnloadFn>(GetProcAddress(dll, "DllCanUnloadNow"));
    CHECK(getClassObject && canUnload && GetProcAddress(dll, "DllRegisterServer") && GetProcAddress(dll, "DllUnregisterServer"));
    if (!getClassObject || !canUnload)
        return 1;

    IClassFactory* factory = nullptr;
    CHECK(getClassObject(IID_IClassFactory, IID_IClassFactory, reinterpret_cast<void**>(&factory)) == CLASS_E_CLASSNOTAVAILABLE);
    CHECK_HR(getClassObject(weq::kClsidApo, IID_IClassFactory, reinterpret_cast<void**>(&factory)));
    if (!factory)
        return 1;

    runFormat(factory, 2, 48000, true, false);
    runFormat(factory, 2, 44100, false, false);
    runFormat(factory, 8, 96000, true, false);
    runFormat(factory, 1, 192000, false, false);
    runFormat(factory, 2, 48000, true, true);
    runFormat(factory, 6, 44100, false, true);

    factory->Release();
    CHECK(canUnload() == S_OK);

    std::printf("\n%s (%d failed checks)\n", g_failures ? "FAILED" : "all APO smoke checks passed", g_failures);
    CoUninitialize();
    return g_failures ? 1 : 0;
}
