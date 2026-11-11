#include "eqdsp/AdaptiveResampler.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace eqdsp {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kKaiserBeta = 10.06; // ~100 dB stopband
constexpr double kCutoffFactor = 0.97;

// Zeroth-order modified Bessel function of the first kind.
double besselI0(double x)
{
    double sum = 1.0, term = 1.0;
    const double q = 0.25 * x * x;
    for (int k = 1; k < 64; ++k) {
        term *= q / (static_cast<double>(k) * k);
        sum += term;
        if (term < sum * 1e-17)
            break;
    }
    return sum;
}

} // namespace

bool AdaptiveResampler::prepare(int channels, double inRate, double outRate, int maxOutFrames, int halfTaps, int phases)
{
    if (channels < 1 || !(inRate > 0) || !(outRate > 0) || maxOutFrames < 1 || halfTaps < 4 || phases < 16)
        return false;

    channels_ = channels;
    maxOut_ = maxOutFrames;
    nominalStep_ = inRate / outRate;
    step_ = nominalStep_;

    // Downsampling: lower the cutoff below the output Nyquist and lengthen the
    // kernel proportionally so the transition band stays the same at the output.
    const double down = std::max(1.0, nominalStep_);
    cutoff_ = 0.5 * kCutoffFactor / down;
    half_ = static_cast<int>(std::ceil(halfTaps * down));
    taps_ = 2 * half_;
    phases_ = phases;

    buildTable();
    blend_.assign(static_cast<size_t>(taps_), 0.0);

    const int maxIn = static_cast<int>(std::ceil(maxOutFrames * nominalStep_ * 1.02)) + 4;
    capacityFrames_ = taps_ + maxIn + 8;
    buf_.assign(static_cast<size_t>(capacityFrames_) * channels_, 0.0f);
    reset();
    return true;
}

void AdaptiveResampler::buildTable()
{
    // Row p holds the kernel for fractional offset p / phases_, tap j at
    // integer offset k = j - half_ + 1 relative to floor(pos).
    table_.assign(static_cast<size_t>(phases_ + 1) * taps_, 0.0f);
    const double i0Beta = besselI0(kKaiserBeta);
    std::vector<double> row(static_cast<size_t>(taps_));
    for (int p = 0; p <= phases_; ++p) {
        const double frac = static_cast<double>(p) / phases_;
        double sum = 0.0;
        for (int j = 0; j < taps_; ++j) {
            const double t = (j - half_ + 1) - frac; // input-sample distance from the output instant
            const double x = 2.0 * cutoff_ * t;
            const double sinc = std::fabs(x) < 1e-12 ? 1.0 : std::sin(kPi * x) / (kPi * x);
            const double r = t / half_;
            const double w = std::fabs(r) >= 1.0 ? 0.0 : besselI0(kKaiserBeta * std::sqrt(1.0 - r * r)) / i0Beta;
            row[static_cast<size_t>(j)] = 2.0 * cutoff_ * sinc * w;
            sum += row[static_cast<size_t>(j)];
        }
        // Unity DC gain for every phase.
        for (int j = 0; j < taps_; ++j)
            table_[static_cast<size_t>(p) * taps_ + j] = static_cast<float>(row[static_cast<size_t>(j)] / sum);
    }
}

void AdaptiveResampler::reset()
{
    // half_ - 1 frames of silent history, then the read position sits on the
    // first real input frame.
    std::fill(buf_.begin(), buf_.end(), 0.0f);
    bufFrames_ = half_ - 1;
    pos_ = static_cast<double>(half_ - 1);
    missing_ = 0;
}

void AdaptiveResampler::setCorrection(double correction)
{
    step_ = nominalStep_ * std::clamp(correction, 0.98, 1.02);
}

double AdaptiveResampler::bufferedInputFrames() const
{
    return static_cast<double>(bufFrames_) - pos_;
}

int AdaptiveResampler::inputFramesNeeded(int outFrames) const
{
    if (outFrames <= 0)
        return 0;
    const double lastPos = pos_ + (outFrames - 1) * step_;
    const int needTotal = static_cast<int>(std::floor(lastPos)) + half_ + 1;
    return std::max(0, std::min(needTotal - bufFrames_, capacityFrames_ - bufFrames_));
}

float* AdaptiveResampler::inputWritePointer()
{
    return buf_.data() + static_cast<size_t>(bufFrames_) * channels_;
}

void AdaptiveResampler::commitInput(int frames, int validFrames)
{
    frames = std::clamp(frames, 0, capacityFrames_ - bufFrames_);
    validFrames = std::clamp(validFrames, 0, frames);
    if (validFrames < frames) {
        std::fill(buf_.begin() + static_cast<ptrdiff_t>(bufFrames_ + validFrames) * channels_,
                  buf_.begin() + static_cast<ptrdiff_t>(bufFrames_ + frames) * channels_, 0.0f);
        missing_ += frames - validFrames;
    }
    bufFrames_ += frames;
}

void AdaptiveResampler::produce(float* out, int outFrames)
{
    const int ch = channels_;
    for (int n = 0; n < outFrames; ++n) {
        const double fi = std::floor(pos_);
        int i = static_cast<int>(fi);
        const double frac = pos_ - fi;

        if (i + half_ >= bufFrames_) {
            // Not enough input (capacity clamp hit): emit silence for the rest.
            std::fill(out + static_cast<size_t>(n) * ch, out + static_cast<size_t>(outFrames) * ch, 0.0f);
            break;
        }

        const double f = frac * phases_;
        const int p = std::min(static_cast<int>(f), phases_ - 1);
        const double a = f - p;
        const float* r0 = &table_[static_cast<size_t>(p) * taps_];
        const float* r1 = r0 + taps_;
        for (int j = 0; j < taps_; ++j)
            blend_[static_cast<size_t>(j)] = r0[j] + a * (static_cast<double>(r1[j]) - r0[j]);

        const float* x = &buf_[static_cast<size_t>(i - half_ + 1) * ch];
        for (int c = 0; c < ch; ++c) {
            double acc = 0.0;
            for (int j = 0; j < taps_; ++j)
                acc += blend_[static_cast<size_t>(j)] * x[static_cast<size_t>(j) * ch + c];
            out[static_cast<size_t>(n) * ch + c] = static_cast<float>(acc);
        }
        pos_ += step_;
    }

    // Drop history that no future output can reach.
    const int keepFrom = static_cast<int>(std::floor(pos_)) - half_ + 1;
    if (keepFrom > 0) {
        const int drop = std::min(keepFrom, bufFrames_);
        std::memmove(buf_.data(), buf_.data() + static_cast<size_t>(drop) * ch,
                     sizeof(float) * static_cast<size_t>(bufFrames_ - drop) * ch);
        bufFrames_ -= drop;
        pos_ -= drop;
    }
}

// ------------------------------------------------------------------ DriftController

void DriftController::prepare(double inRate, double targetFrames, double timeConstantSec, double smoothingSec,
                              double maxPpm, double integratorBandFrames)
{
    band_ = std::max(0.0, integratorBandFrames);
    // Linearised loop: d(err)/dt = inRate * (drift - u), u = kp*err + ki*integral(err).
    // Critically damped with closed-loop time constant tau.
    const double tau = std::max(0.1, timeConstantSec);
    kp_ = 2.0 / (tau * inRate);
    ki_ = 1.0 / (tau * tau * inRate);
    smoothSec_ = std::max(1e-3, smoothingSec);
    maxU_ = std::max(1.0, maxPpm) * 1e-6;
    target_ = targetFrames;
    reset();
}

void DriftController::reset(bool keepRate)
{
    smoothed_ = target_;
    if (!keepRate)
        integral_ = 0.0;
    u_ = std::clamp(ki_ * integral_, -maxU_, maxU_);
    primed_ = false;
}

double DriftController::update(double fillFrames, double dtSec)
{
    if (!primed_) {
        smoothed_ = fillFrames;
        primed_ = true;
    } else {
        const double a = 1.0 - std::exp(-std::max(0.0, dtSec) / smoothSec_);
        smoothed_ += (fillFrames - smoothed_) * a;
    }
    const double err = smoothed_ - target_;

    // Integrate, but don't wind up while saturated in the same direction.
    const double uP = kp_ * err;
    const double errI = band_ > 0.0 ? std::clamp(err, -band_, band_) : err;
    const double candidate = integral_ + errI * dtSec;
    const double uTry = uP + ki_ * candidate;
    if (std::fabs(uTry) < maxU_ || (uTry > 0) != (err > 0))
        integral_ = candidate;

    u_ = std::clamp(uP + ki_ * integral_, -maxU_, maxU_);
    return 1.0 + u_;
}

} // namespace eqdsp
