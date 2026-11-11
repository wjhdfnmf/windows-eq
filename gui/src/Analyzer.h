#pragma once

#include <complex>
#include <vector>

namespace gui {

// Real-time spectrum of the signal before and after the EQ. Fed with the
// router's {pre, post} mono pairs; produces smoothed per-bin levels in dBFS
// (a full-scale sine reads 0 dB), with a +3 dB/octave tilt around 1 kHz so
// music looks roughly flat.
class Analyzer {
public:
    static constexpr int kSize = 8192;

    Analyzer();
    void setSampleRate(double rate);
    double sampleRate() const { return rate_; }
    void push(const float* prePost, size_t frames);
    // Runs at most one transform per call when enough new audio has arrived.
    // Returns true when the spectra changed.
    bool update(double dtSec);
    void clear();
    void setComputePre(bool on) { computePre_ = on; }

    int bins() const { return kSize / 2; }
    double binHz() const { return rate_ / kSize; }
    const std::vector<float>& pre() const { return preDb_; }
    const std::vector<float>& post() const { return postDb_; }
    // Level of the bins covering [f0, f1] (max), in tilted dB.
    float levelAt(const std::vector<float>& spectrum, double f0, double f1) const;
    bool hasSignal() const { return hasSignal_; }

private:
    void transform(const std::vector<float>& history, std::vector<double>& power);

    double rate_ = 48000;
    std::vector<float> histPre_, histPost_; // circular, kSize
    size_t writePos_ = 0;
    size_t sinceLast_ = 0;
    std::vector<float> window_;
    std::vector<double> re_, im_, twRe_, twIm_, tilt_;
    std::vector<unsigned> bitrev_;
    std::vector<double> powPre_, powPost_, scratch_;
    std::vector<float> preDb_, postDb_;
    double windowSum_ = 1;
    bool hasSignal_ = false;
    bool computePre_ = true;
    double silentFor_ = 0;
};

} // namespace gui
