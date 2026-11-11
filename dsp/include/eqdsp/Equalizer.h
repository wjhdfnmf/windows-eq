#pragma once

#include "eqdsp/Types.h"

#include <vector>

namespace eqdsp {

// Multichannel parametric EQ: a cascade of up to kMaxBands biquads per channel,
// then preamp gain, then an optional safety peak limiter.
//
// - Transposed Direct Form II, 64-bit double state and math, float I/O.
// - SSE2: channels are processed in pairs (one __m128d lane each).
// - Parameter changes ramp the coefficients per sample (no clicks/zipper).
// - Bands at 0 dB are skipped, and a fully flat EQ is a bit-exact passthrough.
// - Zero latency.
//
// Threading: prepare() allocates and must not be called on the audio thread.
// setSettings(), process() and reset() are real-time safe (no allocation, no
// locks, no syscalls) but must be called from one thread at a time.
class Equalizer {
public:
    Equalizer();
    ~Equalizer();
    Equalizer(const Equalizer&) = delete;
    Equalizer& operator=(const Equalizer&) = delete;

    // Returns false for unsupported formats. Keeps the current settings.
    bool prepare(double sampleRate, int numChannels, double rampMs = 20.0);

    // immediate == true jumps straight to the new coefficients (use when not
    // playing). Otherwise changes are smoothed over the ramp time.
    void setSettings(const EqSettings& settings, bool immediate = false);
    const EqSettings& settings() const { return settings_; }

    // Interleaved float audio. in == out (in-place) is allowed.
    void process(const float* in, float* out, int frames);

    // Clears filter state and finishes any ramps.
    void reset();

    double sampleRate() const { return sampleRate_; }
    int numChannels() const { return numChannels_; }
    int latencyFrames() const { return 0; }

    // True when the current state passes audio through unchanged.
    bool isTransparent() const;

    // Deepest limiter gain reduction (dB, <= 0) since the last call.
    double takeLimiterReductionDb();

    struct Stage; // defined in Equalizer.cpp

private:
    void applyTargets(bool immediate);
    void processBlock(const float* in, float* out, int frames);

    EqSettings settings_{};
    double sampleRate_ = 0.0;
    int numChannels_ = 0;
    int numPairs_ = 0;
    int rampSamples_ = 1;
    bool prepared_ = false;

    std::vector<Stage> stages_;   // [pair * kMaxBands + band]
    std::vector<double> buffer_;  // [pair][frame][lane]

    double gain_ = 1.0, gainTarget_ = 1.0, gainDelta_ = 0.0;
    int gainRampLeft_ = 0;

    double limiterCeiling_ = 1.0;
    double limiterGain_ = 1.0;
    double limiterRelease_ = 0.0;
    double limiterMinGain_ = 1.0;
};

} // namespace eqdsp
