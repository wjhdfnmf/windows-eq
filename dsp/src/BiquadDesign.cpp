#include "eqdsp/BiquadDesign.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace eqdsp {

namespace {

constexpr double kPi = 3.14159265358979323846;

bool isFinite(double v) { return std::isfinite(v); }

} // namespace

BiquadCoeffs designBiquad(const Band& band, double sampleRate)
{
    BiquadCoeffs out; // identity

    if (!band.enabled || !(sampleRate > 0.0) || !isFinite(sampleRate))
        return out;
    if (!isFinite(band.frequencyHz) || !isFinite(band.q) || !isFinite(band.gainDb))
        return out;

    const double nyquist = 0.5 * sampleRate;
    const double f = std::clamp(band.frequencyHz, 1.0, nyquist * 0.9998);
    const double q = std::clamp(band.q, 0.01, 100.0);
    const double gainDb = std::clamp(band.gainDb, -48.0, 48.0);

    const bool gainType = band.type == FilterType::Peaking ||
                          band.type == FilterType::LowShelf ||
                          band.type == FilterType::HighShelf;
    if (gainType && gainDb == 0.0)
        return out; // exactly flat

    const double w0 = 2.0 * kPi * f / sampleRate;
    const double cw = std::cos(w0);
    const double sw = std::sin(w0);
    // 1 - cos(w0) computed without cancellation (matters for low f at high fs).
    const double sh = std::sin(0.5 * w0);
    const double oneMinusCw = 2.0 * sh * sh;
    const double onePlusCw = 2.0 - oneMinusCw;
    const double alpha = sw / (2.0 * q);
    const double A = std::pow(10.0, gainDb / 40.0);

    double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;

    switch (band.type) {
    case FilterType::Peaking:
        b0 = 1.0 + alpha * A;
        b1 = -2.0 * cw;
        b2 = 1.0 - alpha * A;
        a0 = 1.0 + alpha / A;
        a1 = -2.0 * cw;
        a2 = 1.0 - alpha / A;
        break;

    case FilterType::LowShelf: {
        const double s2a = 2.0 * std::sqrt(A) * alpha;
        const double Ap1 = A + 1.0, Am1 = A - 1.0;
        b0 = A * (Ap1 - Am1 * cw + s2a);
        b1 = 2.0 * A * (Am1 - Ap1 * cw);
        b2 = A * (Ap1 - Am1 * cw - s2a);
        a0 = Ap1 + Am1 * cw + s2a;
        a1 = -2.0 * (Am1 + Ap1 * cw);
        a2 = Ap1 + Am1 * cw - s2a;
        break;
    }

    case FilterType::HighShelf: {
        const double s2a = 2.0 * std::sqrt(A) * alpha;
        const double Ap1 = A + 1.0, Am1 = A - 1.0;
        b0 = A * (Ap1 + Am1 * cw + s2a);
        b1 = -2.0 * A * (Am1 + Ap1 * cw);
        b2 = A * (Ap1 + Am1 * cw - s2a);
        a0 = Ap1 - Am1 * cw + s2a;
        a1 = 2.0 * (Am1 - Ap1 * cw);
        a2 = Ap1 - Am1 * cw - s2a;
        break;
    }

    case FilterType::LowPass:
        b0 = 0.5 * oneMinusCw;
        b1 = oneMinusCw;
        b2 = 0.5 * oneMinusCw;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cw;
        a2 = 1.0 - alpha;
        break;

    case FilterType::HighPass:
        b0 = 0.5 * onePlusCw;
        b1 = -onePlusCw;
        b2 = 0.5 * onePlusCw;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cw;
        a2 = 1.0 - alpha;
        break;

    case FilterType::BandPass:
        b0 = alpha;
        b1 = 0.0;
        b2 = -alpha;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cw;
        a2 = 1.0 - alpha;
        break;

    case FilterType::Notch:
        b0 = 1.0;
        b1 = -2.0 * cw;
        b2 = 1.0;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cw;
        a2 = 1.0 - alpha;
        break;

    case FilterType::AllPass:
        b0 = 1.0 - alpha;
        b1 = -2.0 * cw;
        b2 = 1.0 + alpha;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cw;
        a2 = 1.0 - alpha;
        break;

    default:
        return out;
    }

    const double inv = 1.0 / a0;
    out.b0 = b0 * inv;
    out.b1 = b1 * inv;
    out.b2 = b2 * inv;
    out.a1 = a1 * inv;
    out.a2 = a2 * inv;

    if (!isFinite(out.b0) || !isFinite(out.b1) || !isFinite(out.b2) ||
        !isFinite(out.a1) || !isFinite(out.a2))
        return BiquadCoeffs{};
    return out;
}

double biquadMagnitudeDb(const BiquadCoeffs& c, double freqHz, double sampleRate)
{
    const double w = 2.0 * kPi * freqHz / sampleRate;
    const std::complex<double> z1 = std::polar(1.0, -w);
    const std::complex<double> z2 = z1 * z1;
    const std::complex<double> num = c.b0 + c.b1 * z1 + c.b2 * z2;
    const std::complex<double> den = 1.0 + c.a1 * z1 + c.a2 * z2;
    const double mag = std::abs(num) / std::abs(den);
    return 20.0 * std::log10(std::max(mag, 1e-30));
}

double responseDb(const EqSettings& s, int channel, double freqHz, double sampleRate,
                  bool includePreamp)
{
    if (s.bypass)
        return 0.0;

    double db = includePreamp ? s.preampDb : 0.0;
    const uint32_t bit = (channel >= 0 && channel < kMaxChannels) ? (1u << channel) : 0u;
    const int n = std::min<int>(static_cast<int>(s.numBands), kMaxBands);
    for (int i = 0; i < n; ++i) {
        const Band& b = s.bands[i];
        if (!(b.channelMask & bit))
            continue;
        const BiquadCoeffs c = designBiquad(b, sampleRate);
        if (!c.isIdentity())
            db += biquadMagnitudeDb(c, freqHz, sampleRate);
    }
    return db;
}

double maxBoostDb(const EqSettings& s, int numChannels, double sampleRate)
{
    if (s.bypass)
        return 0.0;

    constexpr int kPoints = 2048;
    const double fLo = 10.0;
    const double fHi = 0.5 * sampleRate * 0.9998;
    const double ratio = std::log(fHi / fLo);

    double maxDb = -1e300;
    const int chans = std::clamp(numChannels, 1, kMaxChannels);
    for (int ch = 0; ch < chans; ++ch) {
        for (int i = 0; i < kPoints; ++i) {
            const double f = fLo * std::exp(ratio * i / (kPoints - 1));
            maxDb = std::max(maxDb, responseDb(s, ch, f, sampleRate, false));
        }
    }
    return maxDb;
}

double autoPreampDb(const EqSettings& s, int numChannels, double sampleRate)
{
    return -std::max(0.0, maxBoostDb(s, numChannels, sampleRate));
}

} // namespace eqdsp
