#include "Router.h"

#include "AudioDevices.h"

#include <avrt.h>
#include <mmreg.h>
#include <timeapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace weq {

namespace {

constexpr GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr REFERENCE_TIME kCaptureBuffer100ns = 200000; // 20 ms

template <typename T>
void safeRelease(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

WAVEFORMATEXTENSIBLE floatFormatLike(const WAVEFORMATEX* mix)
{
    WAVEFORMATEXTENSIBLE f{};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = mix->nChannels;
    f.Format.nSamplesPerSec = mix->nSamplesPerSec;
    f.Format.wBitsPerSample = 32;
    f.Format.nBlockAlign = static_cast<WORD>(mix->nChannels * 4);
    f.Format.nAvgBytesPerSec = f.Format.nSamplesPerSec * f.Format.nBlockAlign;
    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = 32;
    if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22)
        f.dwChannelMask = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix)->dwChannelMask;
    else
        f.dwChannelMask = mix->nChannels == 1 ? SPEAKER_FRONT_CENTER
                        : mix->nChannels == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
    f.SubFormat = kSubtypeFloat;
    return f;
}

struct MmcssScope {
    HANDLE task = nullptr;
    MmcssScope()
    {
        DWORD index = 0;
        task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &index);
    }
    ~MmcssScope()
    {
        if (task)
            AvRevertMmThreadCharacteristics(task);
    }
};

struct ComScope {
    ComScope() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~ComScope() { CoUninitialize(); }
};

void atomicMax(std::atomic<float>& a, float v)
{
    float cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}

} // namespace

Router::Router()
{
    analysis_.init(32768, 2);
}

Router::~Router()
{
    stop();
}

long long Router::now100ns()
{
    static const LONGLONG freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<long long>(static_cast<double>(c.QuadPart) * 1e7 / static_cast<double>(freq));
}

std::wstring Router::failure() const
{
    std::lock_guard<std::mutex> lock(failMutex_);
    return failure_;
}

void Router::fail(const std::wstring& what, HRESULT hr)
{
    {
        std::lock_guard<std::mutex> lock(failMutex_);
        if (failure_.empty())
            failure_ = what + L": " + hrString(hr);
    }
    failed_.store(true);
    stop_.store(true);
}

bool Router::start(const RouterConfig& config, std::wstring& error)
{
    stop();

    // Two routers on the same devices would play everything twice.
    instanceMutex_ = CreateMutexW(nullptr, FALSE, L"Local\\WindowsEQ.Router");
    if (instanceMutex_ && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(instanceMutex_);
        instanceMutex_ = nullptr;
        error = L"another Windows EQ router is already running (close the other window or weq-router first)";
        return false;
    }

    config_ = config;
    stop_.store(false);
    failed_.store(false);
    failure_.clear();
    stats_.running.store(false);
    stats_.underruns.store(0);
    stats_.overrunFrames.store(0);
    stats_.discontinuities.store(0);
    stats_.resyncs.store(0);
    stats_.silenceTrimmedFrames.store(0);
    stats_.silencePaddedFrames.store(0);
    silentTail_.store(0);

    auto bail = [&](const std::wstring& what, HRESULT hr) {
        error = what + L": " + hrString(hr);
        stop();
        return false;
    };

    // ---- capture stream
    IMMDevice* capDev = openDevice(config.captureId, error);
    if (!capDev)
        return false;
    HRESULT hr = capDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&capClient_));
    capDev->Release();
    if (FAILED(hr))
        return bail(L"activate capture device", hr);

    WAVEFORMATEX* mix = nullptr;
    if (FAILED(hr = capClient_->GetMixFormat(&mix)))
        return bail(L"capture mix format", hr);
    WAVEFORMATEXTENSIBLE capFmt = floatFormatLike(mix);
    CoTaskMemFree(mix);

    DWORD capFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                     AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    if (config.loopback)
        capFlags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
    hr = capClient_->Initialize(AUDCLNT_SHAREMODE_SHARED, capFlags, kCaptureBuffer100ns, 0,
                                reinterpret_cast<WAVEFORMATEX*>(&capFmt), nullptr);
    if (FAILED(hr))
        return bail(L"initialise capture stream", hr);
    capEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(hr = capClient_->SetEventHandle(capEvent_)))
        return bail(L"capture event", hr);
    if (FAILED(hr = capClient_->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture_))))
        return bail(L"capture service", hr);

    capRate_ = static_cast<int>(capFmt.Format.nSamplesPerSec);
    capCh_ = capFmt.Format.nChannels;
    REFERENCE_TIME defPeriod = 0, minPeriod = 0;
    capClient_->GetDevicePeriod(&defPeriod, &minPeriod);
    capPeriodFrames_ = static_cast<UINT32>(std::lround(defPeriod * 1e-7 * capRate_));

    // ---- render stream
    IMMDevice* renDev = openDevice(config.renderId, error);
    if (!renDev) {
        stop();
        return false;
    }
    hr = renDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&renClient_));
    renDev->Release();
    if (FAILED(hr))
        return bail(L"activate output device", hr);
    if (FAILED(hr = renClient_->GetMixFormat(&mix)))
        return bail(L"output mix format", hr);
    WAVEFORMATEXTENSIBLE renFmt = floatFormatLike(mix);
    CoTaskMemFree(mix);

    hr = renClient_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                    AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                0, 0, reinterpret_cast<WAVEFORMATEX*>(&renFmt), nullptr);
    if (FAILED(hr))
        return bail(L"initialise output stream", hr);
    renEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(hr = renClient_->SetEventHandle(renEvent_)))
        return bail(L"output event", hr);
    if (FAILED(hr = renClient_->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render_))))
        return bail(L"output service", hr);
    renClient_->GetBufferSize(&renBufferFrames_);
    renClient_->GetDevicePeriod(&defPeriod, &minPeriod);
    renRate_ = static_cast<int>(renFmt.Format.nSamplesPerSec);
    renCh_ = renFmt.Format.nChannels;
    renPeriodFrames_ = static_cast<UINT32>(std::lround(defPeriod * 1e-7 * renRate_));

    // ---- buffering target (capture frames, mean fill): one capture packet +
    // half a render request + resampler look-ahead + margin. Just before a
    // request the fill is then a full packet + request + look-ahead + margin.
    chunkFrames_ = static_cast<double>(std::max(renPeriodFrames_, 1u)) * capRate_ / renRate_;
    baseFrames_ = capPeriodFrames_ + 64 + config.marginMs * 1e-3 * capRate_;
    targetFrames_ = baseFrames_ + chunkFrames_ / 2;
    ring_.init(static_cast<size_t>(capRate_), capCh_); // 1 s

    stats_.captureRate = capRate_;
    stats_.captureChannels = capCh_;
    stats_.renderRate = renRate_;
    stats_.renderChannels = renCh_;
    stats_.capturePeriodFrames = static_cast<int>(capPeriodFrames_);
    stats_.renderBufferFrames = static_cast<int>(renBufferFrames_);
    stats_.targetMs = targetFrames_ * 1000.0 / capRate_;

    trace_.clear();
    trace_.reserve(4000);
    capThread_ = std::thread(&Router::captureThread, this);
    renThread_ = std::thread(&Router::renderThread, this);
    return true;
}

void Router::stop()
{
    stop_.store(true);
    if (capThread_.joinable())
        capThread_.join();
    if (renThread_.joinable())
        renThread_.join();
    if (capClient_)
        capClient_->Stop();
    if (renClient_)
        renClient_->Stop();
    safeRelease(capture_);
    safeRelease(render_);
    safeRelease(capClient_);
    safeRelease(renClient_);
    if (capEvent_) {
        CloseHandle(capEvent_);
        capEvent_ = nullptr;
    }
    if (renEvent_) {
        CloseHandle(renEvent_);
        renEvent_ = nullptr;
    }
    stats_.running.store(false);
    if (instanceMutex_) {
        CloseHandle(instanceMutex_);
        instanceMutex_ = nullptr;
    }
}

// ------------------------------------------------------------------ capture

void Router::captureThread()
{
    ComScope com;
    MmcssScope mmcss;
    // Loopback has no packets while the source is idle, so this thread paces
    // synthetic silence with timed waits: ask for 1 ms timer resolution.
    struct TimerResolution {
        bool on;
        explicit TimerResolution(bool enable) : on(enable && timeBeginPeriod(1) == TIMERR_NOERROR) {}
        ~TimerResolution() { if (on) timeEndPeriod(1); }
    } timerRes(config_.loopback);

    HRESULT hr = capClient_->Start();
    if (FAILED(hr)) {
        fail(L"start capture", hr);
        return;
    }

    long long lastData = now100ns();
    const long long silenceAfter = static_cast<long long>(3.0 * capPeriodFrames_ * 1e7 / capRate_);
    bool idle = false;

    while (!stop_.load()) {
        WaitForSingleObject(capEvent_, 10);

        bool got = false;
        for (;;) {
            UINT32 packet = 0;
            if (FAILED(hr = capture_->GetNextPacketSize(&packet))) {
                fail(L"capture", hr);
                return;
            }
            if (packet == 0)
                break;

            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 devPos = 0, qpcPos = 0;
            if (FAILED(hr = capture_->GetBuffer(&data, &frames, &flags, &devPos, &qpcPos))) {
                fail(L"capture", hr);
                return;
            }
            const float* src = (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? nullptr : reinterpret_cast<const float*>(data);
            bool silent = true;
            for (size_t i = 0, n = static_cast<size_t>(frames) * capCh_; src && silent && i < n; ++i)
                silent = std::fabs(src[i]) < 1e-6f; // below -120 dBFS
            if (!silent)
                silentTail_.store(0, std::memory_order_release); // before the audio becomes readable
            const size_t written = ring_.write(src, frames);
            if (silent)
                silentTail_.fetch_add(frames, std::memory_order_release);
            stats_.capturedFrames += frames;
            ++stats_.capturePackets;
            if (written < frames)
                stats_.overrunFrames += frames - written;
            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
                ++stats_.discontinuities;
            capture_->ReleaseBuffer(frames);

            // Device timestamp of the packet's last frame (falls back to now).
            const long long end = qpcPos ? static_cast<long long>(qpcPos) +
                                               static_cast<long long>(frames * 1e7 / capRate_)
                                         : now100ns();
            lastPacketFrames_.store(frames, std::memory_order_relaxed);
            lastPacketEnd100ns_.store(end, std::memory_order_release);
            got = true;
        }

        const long long now = now100ns();
        if (got) {
            lastData = now;
            idle = false;
        } else if (config_.loopback && (idle || now - lastData > silenceAfter)) {
            // Loopback delivers nothing while the source is idle; keep the
            // clock running by synthesising silence in real time, in small
            // steps (every wake-up) so the FIFO level stays smooth.
            idle = true;
            const UINT32 frames = static_cast<UINT32>((now - lastData) * 1e-7 * capRate_);
            if (frames > 0) {
                const size_t written = ring_.write(nullptr, frames);
                silentTail_.fetch_add(frames, std::memory_order_release);
                stats_.synthesizedFrames += frames;
                if (written < frames)
                    stats_.overrunFrames += frames - written;
                lastPacketFrames_.store(frames, std::memory_order_relaxed);
                lastPacketEnd100ns_.store(now, std::memory_order_release);
                lastData += static_cast<long long>(frames * 1e7 / capRate_);
            }
        }
    }
}

// ------------------------------------------------------------------ render

void Router::mapChannels(const float* in, int inCh, float* out, int outCh, int frames)
{
    if (inCh == outCh) {
        std::memcpy(out, in, sizeof(float) * static_cast<size_t>(frames) * outCh);
        return;
    }
    for (int f = 0; f < frames; ++f) {
        const float* i = in + static_cast<size_t>(f) * inCh;
        float* o = out + static_cast<size_t>(f) * outCh;
        if (inCh == 1) {
            for (int c = 0; c < outCh; ++c)
                o[c] = c < 2 ? i[0] : 0.0f;
        } else if (outCh == 1) {
            o[0] = 0.5f * (i[0] + i[1]);
        } else {
            for (int c = 0; c < outCh; ++c)
                o[c] = c < inCh ? i[c] : 0.0f;
        }
    }
}

void Router::renderThread()
{
    ComScope com;
    MmcssScope mmcss;

    eqdsp::Equalizer eq;
    eq.prepare(renRate_, capCh_);
    eqdsp::EqSettings settings;
    uint32_t settingsSeq = 0;
    if (mailbox_.tryTake(settings, settingsSeq))
        eq.setSettings(settings, true);

    eqdsp::AdaptiveResampler rs;
    rs.prepare(capCh_, capRate_, renRate_, static_cast<int>(renBufferFrames_));
    eqdsp::DriftController dc;
    // Up to ±5000 ppm (8.6 cents): the output's effective consumption rate can
    // be well over 1000 ppm off the system clock that virtual cables run on,
    // especially while the stream settles (seen on the Focusrite WDM driver).
    // Integrate at most one capture packet of error: output stalls step the
    // fill by whole packets, and a run of them must not wind up the rate
    // estimate. Real clock offsets up to ~1300 ppm stay inside the band.
    dc.prepare(capRate_, targetFrames_, 15.0, 1.0, 5000.0, static_cast<double>(capPeriodFrames_));
    auto retarget = [&] {
        targetFrames_ = baseFrames_ + chunkFrames_ / 2;
        dc.setTarget(targetFrames_);
        stats_.targetMs = targetFrames_ * 1000.0 / capRate_;
    };

    std::vector<float> work(static_cast<size_t>(renBufferFrames_) * capCh_);
    std::vector<float> tap(static_cast<size_t>(renBufferFrames_) * 2); // {pre, post} mono pairs
    auto monoAt = [this](const float* frame) {
        return capCh_ >= 2 ? 0.5f * (frame[0] + frame[1]) : frame[0];
    };
    auto pull = [this](float* dst, int frames) { return static_cast<int>(ring_.read(dst, static_cast<size_t>(frames))); };

    // Pre-roll the output with silence, then start it.
    BYTE* data = nullptr;
    if (SUCCEEDED(render_->GetBuffer(renBufferFrames_, &data)))
        render_->ReleaseBuffer(renBufferFrames_, AUDCLNT_BUFFERFLAGS_SILENT);
    HRESULT hr = renClient_->Start();
    if (FAILED(hr)) {
        fail(L"start output", hr);
        return;
    }

    // Device clock vs system clock, for diagnostics.
    IAudioClock* clock = nullptr;
    renClient_->GetService(__uuidof(IAudioClock), reinterpret_cast<void**>(&clock));
    UINT64 clockFreq = 0, pos0 = 0, qpc0 = 0;
    if (clock)
        clock->GetFrequency(&clockFreq);
    bool clockPrimed = false;

    bool buffering = true;
    double cpuAvg = 0.0, limiterMin = 0.0;
    int periodsSincePublish = 0;
    LARGE_INTEGER qpf;
    QueryPerformanceFrequency(&qpf);

    while (!stop_.load()) {
        if (WaitForSingleObject(renEvent_, 200) != WAIT_OBJECT_0)
            continue;

        UINT32 padding = 0;
        if (FAILED(hr = renClient_->GetCurrentPadding(&padding))) {
            fail(L"output", hr);
            return;
        }
        const UINT32 avail = renBufferFrames_ - padding;
        if (avail == 0)
            continue;
        if (FAILED(hr = render_->GetBuffer(avail, &data))) {
            fail(L"output", hr);
            return;
        }

        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);

        if (mailbox_.tryTake(settings, settingsSeq))
            eq.setSettings(settings);

        // The output sometimes switches to larger requests (the Focusrite
        // driver goes from 10 ms to 20 ms chunks at times): size the target
        // for the largest request seen.
        const double availCap = static_cast<double>(avail) * capRate_ / renRate_;
        if (!buffering && availCap > chunkFrames_ + 1.0 && avail < renBufferFrames_) {
            chunkFrames_ = std::min(availCap, 0.040 * capRate_);
            retarget();
        }

        // Continuous fill estimate in capture frames (see DriftController),
        // taken half-way through this request so that it does not depend on
        // how big the request is.
        const long long sincePacket = now100ns() - lastPacketEnd100ns_.load(std::memory_order_acquire);
        const double accrued = std::clamp(sincePacket * 1e-7 * capRate_, 0.0,
                                          static_cast<double>(lastPacketFrames_.load(std::memory_order_relaxed)));
        const double fifo = static_cast<double>(ring_.readAvailable());
        const double fill = fifo + rs.bufferedInputFrames() + accrued - availCap / 2;
        if (trace_.size() < trace_.capacity())
            trace_.push_back({static_cast<double>(t0.QuadPart) / qpf.QuadPart, fifo, accrued, rs.bufferedInputFrames(),
                              fill, dc.smoothedFill(), dc.ppm(), avail, padding, buffering});

        if (buffering) {
            // Start-up or after an underrun: play silence until the FIFO is at target.
            const double startFill = fifo + accrued - availCap / 2;
            if (startFill < targetFrames_) {
                render_->ReleaseBuffer(avail, AUDCLNT_BUFFERFLAGS_SILENT);
                continue;
            }
            // Start exactly at the target: drop whatever overshot while we
            // waited (output was silent, so this is inaudible), then process
            // this very period so no extra packet is left behind.
            const double excess = startFill - targetFrames_;
            if (excess >= 1.0)
                ring_.read(nullptr, static_cast<size_t>(excess));
            buffering = false;
            rs.reset();
            dc.reset(true); // the clocks' rate offset survives a re-buffer
            stats_.running.store(true);
        }
        double fillNow =
            static_cast<double>(ring_.readAvailable()) + rs.bufferedInputFrames() + accrued - availCap / 2;

        // Way too much buffered (e.g. after a stall): drop back to the target.
        if (fillNow > targetFrames_ + 0.1 * capRate_) {
            const size_t drop = static_cast<size_t>(fillNow - targetFrames_);
            fillNow -= static_cast<double>(ring_.read(nullptr, drop));
            dc.reset();
            ++stats_.resyncs;
        }

        // While everything queued is digital silence, fix a fill error
        // directly instead of through the slow rate controller: drop excess
        // input, or insert silent output when short. Both are inaudible.
        // Output stalls (the Focusrite driver skips periods, then sometimes
        // catches up with a burst) cause such errors. Decide on the 1 s
        // smoothed fill so a stall followed by a catch-up does nothing, and
        // correct both directions so repeated cycles cannot bias the rate.
        int padOut = 0; // output frames of inserted silence this period
        {
            const double err = dc.smoothedFill() - targetFrames_;
            const double band = 0.5 * capPeriodFrames_;
            if (std::fabs(err) > band) {
                const size_t queued = ring_.readAvailable(); // read before silentTail_
                const bool allSilent = silentTail_.load(std::memory_order_acquire) >= queued;
                if (allSilent && err > 0) {
                    const size_t excess = std::min(static_cast<size_t>(err), queued);
                    ring_.read(nullptr, excess);
                    fillNow -= static_cast<double>(excess);
                    dc.shiftFill(-static_cast<double>(excess));
                    stats_.silenceTrimmedFrames += excess;
                } else if (allSilent && err < 0) {
                    padOut = std::min(static_cast<int>(-err * renRate_ / capRate_), static_cast<int>(avail) / 2);
                    const double padIn = padOut * static_cast<double>(capRate_) / renRate_;
                    fillNow += padIn;
                    dc.shiftFill(padIn);
                    stats_.silencePaddedFrames += static_cast<unsigned long long>(padIn);
                }
            }
        }

        rs.setCorrection(dc.update(fillNow, static_cast<double>(avail) / renRate_));
        const long long missingBefore = rs.missingInputFrames();
        if (padOut > 0)
            std::fill(work.begin(), work.begin() + static_cast<size_t>(padOut) * capCh_, 0.0f);
        rs.process(work.data() + static_cast<size_t>(padOut) * capCh_, static_cast<int>(avail) - padOut, pull);
        if (rs.missingInputFrames() != missingBefore) {
            ++stats_.underruns;
            buffering = true; // re-buffer instead of crawling back slowly
            // The source is burstier than the margin allows: grow the target
            // by 2 ms per underrun (capped at 80 ms) so it self-tunes.
            // An output that ran completely dry (the whole system stalled
            // and nothing got scheduled) says nothing about the source, so
            // don't grow the target for it.
            const bool outputRanDry = avail >= renBufferFrames_;
            if (!outputRanDry && targetFrames_ < 0.080 * capRate_) {
                baseFrames_ += 0.002 * capRate_;
                retarget();
            }
        }

        for (UINT32 f = 0; f < avail; ++f)
            tap[2 * f] = monoAt(&work[static_cast<size_t>(f) * capCh_]);
        eq.process(work.data(), work.data(), static_cast<int>(avail));
        for (UINT32 f = 0; f < avail; ++f)
            tap[2 * f + 1] = monoAt(&work[static_cast<size_t>(f) * capCh_]);
        analysis_.write(tap.data(), avail); // drops what doesn't fit; never blocks
        mapChannels(work.data(), capCh_, reinterpret_cast<float*>(data), renCh_, static_cast<int>(avail));

        float peak = 0.0f, peakCh[2] = {0.0f, 0.0f};
        const float* outF = reinterpret_cast<const float*>(data);
        for (UINT32 f = 0; f < avail; ++f) {
            for (int c = 0; c < renCh_; ++c) {
                const float a = std::fabs(outF[static_cast<size_t>(f) * renCh_ + c]);
                peak = std::max(peak, a);
                if (c < 2)
                    peakCh[c] = std::max(peakCh[c], a);
            }
        }
        atomicMax(stats_.peakCh[0], peakCh[0]);
        atomicMax(stats_.peakCh[1], renCh_ >= 2 ? peakCh[1] : peakCh[0]);

        if (FAILED(hr = render_->ReleaseBuffer(avail, 0))) {
            fail(L"output", hr);
            return;
        }
        stats_.renderedFrames += avail;

        QueryPerformanceCounter(&t1);
        const double busy = static_cast<double>(t1.QuadPart - t0.QuadPart) / qpf.QuadPart;
        const double periodSec = static_cast<double>(avail) / renRate_;
        cpuAvg += (busy / periodSec - cpuAvg) * 0.05;
        limiterMin = std::min(limiterMin, eq.takeLimiterReductionDb());
        atomicMax(stats_.peak, peak);

        if (++periodsSincePublish >= 10) {
            periodsSincePublish = 0;
            if (clock && clockFreq) {
                UINT64 pos = 0, qpc = 0;
                if (SUCCEEDED(clock->GetPosition(&pos, &qpc))) {
                    if (!clockPrimed) {
                        pos0 = pos;
                        qpc0 = qpc;
                        clockPrimed = true;
                    } else if (qpc > qpc0) {
                        const double span = (qpc - qpc0) * 1e-7;
                        const double devSec = static_cast<double>(pos - pos0) / clockFreq;
                        stats_.renderClockPpm = (devSec / span - 1.0) * 1e6;
                        stats_.renderClockSpanSec = span;
                    }
                }
            }
            stats_.fillMs = dc.smoothedFill() * 1000.0 / capRate_;
            stats_.ppm = dc.ppm();
            stats_.cpuPercent = cpuAvg * 100.0;
            stats_.limiterDb = limiterMin;
            limiterMin = 0.0;
            // Added latency: buffered input + output buffer + resampler delay.
            stats_.latencyMs = dc.smoothedFill() * 1000.0 / capRate_ +
                               static_cast<double>(renBufferFrames_) * 1000.0 / renRate_ +
                               rs.latencyInputFrames() * 1000.0 / capRate_;
        }
    }
    if (clock)
        clock->Release();
}

} // namespace weq
