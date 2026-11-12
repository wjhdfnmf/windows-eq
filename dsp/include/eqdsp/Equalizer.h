#pragma once

#include "eqdsp/BiquadDesign.h"
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
// - Dynamic bands (Band::dynamic): the band's level, measured on the input
//   through a band-pass / low-pass / high-pass at the band, moves its gain
//   between gainDb and gainDb + rangeDb (threshold, ratio, 6 dB soft knee,
//   attack/release). Channels are linked (max level). Updated every
//   kDynBlock frames with per-sample coefficient interpolation.
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

    // Current gain change of a dynamic band (dB; 0 when idle or not dynamic).
    double dynamicGainDb(int band) const;

    struct Stage; // defined in Equalizer.cpp

    // Dynamics are evaluated every kDynBlock frames; filter coefficients are
    // interpolated per sample in between.
    static constexpr int kDynBlock = 32;

private:
    // Per-band dynamics state.
    struct Dyn {
        bool active = false;
        double scB0 = 1, scB1 = 0, scB2 = 0, scA1 = 0, scA2 = 0; // sidechain filter
        double power = 0;     // smoothed sidechain power (linear)
        double powerCoef = 0; // RMS smoothing per kDynBlock
        double attCoef = 0, relCoef = 0;
        double change = 0;    // current gain change (dB)
        double applied = 0;   // change the stage coefficients were last built for
    };

    void applyTargets(bool immediate);
    void processBlock(const float* in, float* out, int frames);
    void configureDynamics();
    void runSidechains(const float* in, int frames);

    static constexpr int kMaxSub = 256 / kDynBlock; // sub-blocks per internal block

    // Sidechain state of one channel of one dynamic band: the band filter,
    // then a Hilbert pair (two 4-stage all-pass chains ~90 degrees apart)
    // whose combined magnitude is a ripple-free envelope for steady tones.
    struct ScChannel {
        double z1 = 0, z2 = 0;
        double ap[2][4][4] = {}; // [path][stage]{x1, x2, y1, y2}
        double delay = 0;        // one-sample delay on path 0
    };

    Dyn dyn_[kMaxBands];
    std::vector<ScChannel> scState_;         // [band][channel]
    std::vector<BiquadCoeffs> dynCoefs_;     // [band][sub-block] coefficients to ramp to
    std::vector<unsigned char> dynRetarget_; // [band][sub-block] 1 = ramp to dynCoefs_
    bool anyDynamic_ = false;

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
