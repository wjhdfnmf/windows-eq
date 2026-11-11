#pragma once

#include "FloatRing.h"
#include "SettingsMailbox.h"

#include "eqdsp/AdaptiveResampler.h"
#include "eqdsp/Equalizer.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace weq {

struct RouterConfig {
    std::wstring captureId;     // capture endpoint, or a render endpoint when loopback is set
    bool loopback = false;
    std::wstring renderId;
    // Safety margin on top of the packet sizes. VB-Cable sometimes delivers a
    // capture packet a whole period (10 ms) late, so this covers one.
    double marginMs = 10.0;
};

// Written by the audio threads, read by the UI.
struct RouterStats {
    std::atomic<int> captureRate{0}, captureChannels{0}, renderRate{0}, renderChannels{0};
    std::atomic<int> capturePeriodFrames{0}, renderBufferFrames{0};
    std::atomic<double> targetMs{0}, fillMs{0}, ppm{0}, cpuPercent{0}, latencyMs{0};
    std::atomic<float> peak{0};               // max |sample| since last read (exchange to reset)
    std::atomic<float> peakCh[2]{};           // same, output channels 0 and 1 (for meters)
    std::atomic<double> limiterDb{0};
    std::atomic<unsigned long long> underruns{0}, overrunFrames{0}, discontinuities{0}, resyncs{0};
    std::atomic<unsigned long long> silenceTrimmedFrames{0}; // excess silence dropped (inaudible)
    std::atomic<unsigned long long> silencePaddedFrames{0};  // silence inserted when short (inaudible)
    std::atomic<unsigned long long> capturedFrames{0}, synthesizedFrames{0}, renderedFrames{0}, capturePackets{0};
    std::atomic<bool> running{false};          // past start-up buffering
    // Output device clock measured against the system clock (IAudioClock vs QPC).
    std::atomic<double> renderClockPpm{0}, renderClockSpanSec{0};
};

// Captures from one endpoint, runs the EQ, and renders to another, bridging
// the two device clocks with an adaptive resampler.
class Router {
public:
    Router();
    ~Router();
    Router(const Router&) = delete;
    Router& operator=(const Router&) = delete;

    // Opens both streams and starts the audio threads.
    bool start(const RouterConfig& config, std::wstring& error);
    void stop();

    // Per-period diagnostics for the first seconds after start (render thread
    // writes into a preallocated buffer; read only after stop()).
    struct TraceRow {
        double t, fifo, accrued, resampler, fill, smoothed, ppm;
        unsigned avail, padding;
        bool buffering;
    };
    const std::vector<TraceRow>& trace() const { return trace_; }

    bool failed() const { return failed_.load(); }
    std::wstring failure() const;

    SettingsMailbox& settings() { return mailbox_; }
    RouterStats& stats() { return stats_; }

    // Analyzer tap: mono mix of the signal before and after the EQ, as
    // interleaved {pre, post} frames at the output sample rate
    // (stats().renderRate). Single consumer (the UI); frames are dropped
    // when the consumer falls behind.
    size_t readAnalysis(float* dst, size_t maxFrames) { return analysis_.read(dst, maxFrames); }

private:
    void captureThread();
    void renderThread();
    void fail(const std::wstring& what, HRESULT hr);
    static void mapChannels(const float* in, int inCh, float* out, int outCh, int frames);
    static long long now100ns();

    RouterConfig config_;
    SettingsMailbox mailbox_;
    RouterStats stats_;

    IAudioClient* capClient_ = nullptr;
    IAudioCaptureClient* capture_ = nullptr;
    IAudioClient* renClient_ = nullptr;
    IAudioRenderClient* render_ = nullptr;
    HANDLE capEvent_ = nullptr, renEvent_ = nullptr;

    int capRate_ = 0, capCh_ = 0, renRate_ = 0, renCh_ = 0;
    UINT32 capPeriodFrames_ = 0, renPeriodFrames_ = 0, renBufferFrames_ = 0;
    // Buffering target in capture frames, as a *mean* fill (half-way through
    // a render request): baseFrames_ + chunkFrames_ / 2, where chunkFrames_ is
    // the largest render request seen so far.
    double targetFrames_ = 0, baseFrames_ = 0, chunkFrames_ = 0;

    FloatRing ring_;
    FloatRing analysis_;              // allocated once in the constructor, never re-initialised
    HANDLE instanceMutex_ = nullptr;  // one router per session (GUI or CLI)
    std::atomic<long long> lastPacketEnd100ns_{0};
    std::atomic<unsigned> lastPacketFrames_{0};
    // Consecutive digitally silent frames at the tail of the FIFO input
    // (written before a non-silent packet enters the ring, so a reader that
    // sees silentTail_ >= readAvailable() knows the whole FIFO is silence).
    std::atomic<unsigned long long> silentTail_{0};

    std::vector<TraceRow> trace_;
    std::thread capThread_, renThread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> failed_{false};
    mutable std::mutex failMutex_;
    std::wstring failure_;
};

} // namespace weq
