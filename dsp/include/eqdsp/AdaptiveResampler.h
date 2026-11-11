#pragma once

#include <vector>

namespace eqdsp {

// Band-limited, variable-ratio resampler for bridging two audio clocks.
//
// Kaiser-windowed sinc (default 64 taps, ~100 dB stopband) with 1024 phases
// and linear interpolation between neighbouring phases, so the output time
// can sit anywhere between input samples. The step (input frames per output
// frame) is the nominal inRate/outRate times a small correction supplied by a
// DriftController. When downsampling, the cutoff and kernel length scale so
// content above the output Nyquist is rejected.
//
// Pull model: process() asks the provided callback for exactly the input it
// needs. Interleaved float I/O, double accumulation. prepare() allocates;
// process() / setCorrection() are real-time safe.
class AdaptiveResampler {
public:
    bool prepare(int channels, double inRate, double outRate, int maxOutFrames, int halfTaps = 32, int phases = 1024);
    void reset();

    // step = nominal * correction; correction is clamped to [0.98, 1.02].
    void setCorrection(double correction);
    double nominalStep() const { return nominalStep_; }
    double step() const { return step_; }

    // Input frames held internally that have not yet been consumed (the
    // fractional read position counts too). Add this to the upstream FIFO
    // fill to get the total buffered audio.
    double bufferedInputFrames() const;

    // Group delay in input frames.
    int latencyInputFrames() const { return half_; }

    // Underruns: input frames that the pull callback could not supply (zero-filled).
    long long missingInputFrames() const { return missing_; }

    // pull(float* dst, int frames) -> frames actually written (<= frames).
    // Any outFrames is fine; requests larger than maxOutFrames are split.
    template <class Pull>
    void process(float* out, int outFrames, Pull&& pull)
    {
        while (outFrames > 0) {
            const int n = outFrames < maxOut_ ? outFrames : maxOut_;
            const int need = inputFramesNeeded(n);
            if (need > 0) {
                float* dst = inputWritePointer();
                const int got = pull(dst, need);
                commitInput(need, got < 0 ? 0 : got);
            }
            produce(out, n);
            out += static_cast<size_t>(n) * channels_;
            outFrames -= n;
        }
    }

    // Lower-level API (process() is built from these).
    int inputFramesNeeded(int outFrames) const;
    float* inputWritePointer();
    void commitInput(int frames, int validFrames); // zero-fills [validFrames, frames)
    void produce(float* out, int outFrames);

private:
    void buildTable();

    int channels_ = 0;
    int maxOut_ = 1;
    int half_ = 32;        // taps per side
    int taps_ = 64;        // 2 * half_
    int phases_ = 1024;
    double cutoff_ = 0.5;  // cycles per input sample
    double nominalStep_ = 1.0;
    double step_ = 1.0;

    std::vector<float> table_;   // (phases_ + 1) rows x taps_
    std::vector<double> blend_;  // taps_
    std::vector<float> buf_;     // interleaved input history
    int capacityFrames_ = 0;
    int bufFrames_ = 0;
    double pos_ = 0.0;           // read position (input frames) of the next output
    long long missing_ = 0;
};

// PI controller that keeps a FIFO at a target fill by trimming the
// resampling ratio. Critically damped, with an exponential smoother on the
// fill measurement so the correction changes slowly and the pitch change
// stays inaudible (typically a few ppm).
//
// Feed it a *continuous* fill estimate: packet-based capture makes the raw
// fill a sawtooth, so add the frames accrued since the last capture packet
// (elapsed * rate, capped at one packet) before calling update().
class DriftController {
public:
    // integratorBandFrames > 0 clamps the error the integral term sees to
    // ±band. A clock-rate offset shows up as a slowly growing error and is
    // still integrated; a sudden fill step (an output device that stalls for a
    // period) is mostly left to the proportional term, so the integral does
    // not wind up and overshoot into an underrun afterwards.
    void prepare(double inRate, double targetFrames, double timeConstantSec = 15.0, double smoothingSec = 1.0,
                 double maxPpm = 1000.0, double integratorBandFrames = 0.0);
    // keepRate: keep the learned clock-rate offset (the integral), e.g. when
    // re-buffering after an underrun.
    void reset(bool keepRate = false);
    // Move the set point without disturbing the controller state.
    void setTarget(double targetFrames) { target_ = targetFrames; }
    // The fill was changed directly (frames dropped or inserted): move the
    // smoothed estimate with it so the controller doesn't react to the jump.
    void shiftFill(double deltaFrames) { smoothed_ += deltaFrames; }

    // fillFrames: currently buffered input frames; dtSec: time since last call.
    // Returns the correction to pass to AdaptiveResampler::setCorrection().
    double update(double fillFrames, double dtSec);

    double ppm() const { return u_ * 1e6; }
    double smoothedFill() const { return smoothed_; }
    double targetFrames() const { return target_; }

private:
    double kp_ = 0.0, ki_ = 0.0, smoothSec_ = 1.0, maxU_ = 1e-3, band_ = 0.0;
    double target_ = 0.0, smoothed_ = 0.0, integral_ = 0.0, u_ = 0.0;
    bool primed_ = false;
};

} // namespace eqdsp
