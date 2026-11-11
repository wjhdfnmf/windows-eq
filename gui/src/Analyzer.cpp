#include "Analyzer.h"

#include <algorithm>
#include <cmath>

namespace gui {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr float kFloorDb = -140.0f;
} // namespace

Analyzer::Analyzer()
{
    histPre_.assign(kSize, 0.0f);
    histPost_.assign(kSize, 0.0f);
    window_.resize(kSize);
    windowSum_ = 0;
    for (int i = 0; i < kSize; ++i) {
        window_[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * i / (kSize - 1)));
        windowSum_ += window_[i];
    }
    re_.resize(kSize);
    im_.resize(kSize);
    twRe_.resize(kSize / 2);
    twIm_.resize(kSize / 2);
    for (int i = 0; i < kSize / 2; ++i) {
        twRe_[i] = std::cos(-2.0 * kPi * i / kSize);
        twIm_[i] = std::sin(-2.0 * kPi * i / kSize);
    }
    tilt_.assign(kSize / 2, 0.0);
    bitrev_.resize(kSize);
    int bits = 0;
    while ((1 << bits) < kSize)
        ++bits;
    for (unsigned i = 0; i < static_cast<unsigned>(kSize); ++i) {
        unsigned r = 0;
        for (int b = 0; b < bits; ++b)
            r |= ((i >> b) & 1u) << (bits - 1 - b);
        bitrev_[i] = r;
    }
    powPre_.assign(kSize / 2, 0.0);
    powPost_.assign(kSize / 2, 0.0);
    scratch_.assign(kSize / 2, 0.0);
    preDb_.assign(kSize / 2, kFloorDb);
    postDb_.assign(kSize / 2, kFloorDb);
}

void Analyzer::setSampleRate(double rate)
{
    if (rate > 0 && (rate != rate_ || tilt_[1] == 0.0)) {
        rate_ = rate;
        for (int k = 1; k < kSize / 2; ++k)
            tilt_[k] = 3.0 * std::log2(k * rate_ / kSize / 1000.0);
        clear();
    }
}

void Analyzer::clear()
{
    std::fill(histPre_.begin(), histPre_.end(), 0.0f);
    std::fill(histPost_.begin(), histPost_.end(), 0.0f);
    std::fill(powPre_.begin(), powPre_.end(), 0.0);
    std::fill(powPost_.begin(), powPost_.end(), 0.0);
    std::fill(preDb_.begin(), preDb_.end(), kFloorDb);
    std::fill(postDb_.begin(), postDb_.end(), kFloorDb);
    hasSignal_ = false;
}

void Analyzer::push(const float* prePost, size_t frames)
{
    for (size_t i = 0; i < frames; ++i) {
        histPre_[writePos_] = prePost[2 * i];
        histPost_[writePos_] = prePost[2 * i + 1];
        writePos_ = (writePos_ + 1) % kSize;
    }
    sinceLast_ += frames;
}

void Analyzer::transform(const std::vector<float>& history, std::vector<double>& power)
{
    for (int i = 0; i < kSize; ++i) {
        const size_t src = (writePos_ + i) % kSize; // oldest first
        re_[bitrev_[i]] = static_cast<double>(history[src]) * window_[i];
        im_[bitrev_[i]] = 0.0;
    }
    double* re = re_.data();
    double* im = im_.data();
    for (int len = 2; len <= kSize; len <<= 1) {
        const int half = len >> 1, step = kSize / len;
        for (int i = 0; i < kSize; i += len) {
            for (int j = 0; j < half; ++j) {
                const double wr = twRe_[j * step], wi = twIm_[j * step];
                const int a = i + j, b = a + half;
                const double tr = re[b] * wr - im[b] * wi;
                const double ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
            }
        }
    }
    // Amplitude of a sine = 2|X| / sum(w).
    const double scale2 = (2.0 / windowSum_) * (2.0 / windowSum_);
    for (int k = 0; k < kSize / 2; ++k)
        power[k] = (re[k] * re[k] + im[k] * im[k]) * scale2;
}

bool Analyzer::update(double dtSec)
{
    if (tilt_[1] == 0.0)
        setSampleRate(rate_);
    if (sinceLast_ < static_cast<size_t>(kSize / 6))
        return false; // ~28 ms of new audio at 48 kHz between transforms (<= 35/s)
    // Average over the time since the last transform, not one frame.
    const double elapsed = std::max(dtSec, sinceLast_ / rate_);
    sinceLast_ = 0;

    // Exponential averaging in the power domain: smooth but responsive.
    const double alpha = 1.0 - std::exp(-elapsed / 0.09);
    bool any = false;
    auto run = [&](const std::vector<float>& hist, std::vector<double>& pw, std::vector<float>& db) {
        transform(hist, scratch_);
        for (int k = 1; k < kSize / 2; ++k) {
            pw[k] += (scratch_[k] - pw[k]) * alpha;
            const double v = pw[k] > 1e-16 ? 10.0 * std::log10(pw[k]) + tilt_[k] : kFloorDb;
            db[k] = static_cast<float>(std::max<double>(v, kFloorDb));
            if (pw[k] > 1e-12)
                any = true;
        }
        db[0] = kFloorDb;
    };
    if (computePre_)
        run(histPre_, powPre_, preDb_);
    run(histPost_, powPost_, postDb_);
    silentFor_ = any ? 0.0 : silentFor_ + dtSec;
    hasSignal_ = silentFor_ < 0.5;
    return true;
}

float Analyzer::levelAt(const std::vector<float>& s, double f0, double f1) const
{
    const double hz = binHz();
    const double k0 = f0 / hz, k1 = f1 / hz;
    const int n = kSize / 2;
    if (k1 - k0 < 1.0) {
        // Fewer than one bin per pixel: interpolate between neighbours.
        const double k = std::clamp(0.5 * (k0 + k1), 1.0, n - 2.0);
        const int i = static_cast<int>(k);
        const double t = k - i;
        return static_cast<float>(s[i] * (1 - t) + s[i + 1] * t);
    }
    const int i0 = std::clamp(static_cast<int>(std::ceil(k0)), 1, n - 1);
    const int i1 = std::clamp(static_cast<int>(std::floor(k1)), i0, n - 1);
    float m = kFloorDb;
    for (int i = i0; i <= i1; ++i)
        m = std::max(m, s[i]);
    return m;
}

} // namespace gui
