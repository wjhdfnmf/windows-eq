#include "eqdsp/Equalizer.h"
#include "eqdsp/BiquadDesign.h"

#include <emmintrin.h>
#include <xmmintrin.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace eqdsp {

namespace {

constexpr int kBlock = 256; // frames per internal block

// Flush denormals to zero for the duration of process(). Decaying IIR state
// otherwise turns denormal and can cost 100x CPU on silence.
struct FtzDazGuard {
    unsigned int saved;
    FtzDazGuard() : saved(_mm_getcsr()) { _mm_setcsr(saved | 0x8040u); }
    ~FtzDazGuard() { _mm_setcsr(saved); }
};

inline bool vecEqual(__m128d a, __m128d b)
{
    return _mm_movemask_pd(_mm_cmpneq_pd(a, b)) == 0;
}

constexpr double kKneeDb = 6.0;          // soft knee width of the gain computer
constexpr double kRetargetDb = 0.005;    // smaller dynamic changes aren't worth a redesign

// Gain computer: excess level -> gain change (dB, signed like rangeDb).
double dynamicTarget(const Band& b, double levelDb)
{
    const double x = b.dynMode == DynMode::Below ? b.thresholdDb - levelDb : levelDb - b.thresholdDb;
    double over;
    if (x <= -kKneeDb / 2)
        over = 0.0;
    else if (x >= kKneeDb / 2)
        over = x;
    else
        over = (x + kKneeDb / 2) * (x + kKneeDb / 2) / (2 * kKneeDb);
    const double ratio = std::clamp(std::isfinite(b.ratio) ? b.ratio : 1.0, 1.0, 100.0);
    const double amount = over * (1.0 - 1.0 / ratio);
    const double range = std::clamp(std::isfinite(b.rangeDb) ? b.rangeDb : 0.0, -30.0, 30.0);
    return range < 0 ? -std::min(amount, -range) : std::min(amount, range);
}

// Hilbert transformer from two all-pass chains (Olli Niemitalo's design):
// path 0 delayed by one sample and path 1 are 90 degrees apart (within about
// 0.7 degrees) over almost the whole audio band, each with unity gain.
constexpr double kHilbertA[2][4] = {{0.6923878, 0.9360654322959, 0.9882295226860, 0.9987488452737},
                                    {0.4021921162426, 0.8561710882420, 0.9722909545651, 0.9952884791278}};

inline double allpassChain(double x, double (&st)[4][4], const double (&a)[4])
{
    for (int k = 0; k < 4; ++k) {
        const double a2 = a[k] * a[k];
        const double y = a2 * (x + st[k][3]) - st[k][1]; // y[n] = a^2 (x[n] + y[n-2]) - x[n-2]
        st[k][1] = st[k][0];
        st[k][0] = x;
        st[k][3] = st[k][2];
        st[k][2] = y;
        x = y;
    }
    return x;
}

Band withGainChange(const Band& b, double changeDb)
{
    Band out = b;
    out.gainDb = std::clamp(b.gainDb + changeDb, -36.0, 36.0);
    return out;
}

} // namespace

struct alignas(16) Equalizer::Stage {
    __m128d c[5];     // current b0 b1 b2 a1 a2 (lane 0 / lane 1 = channel pair)
    __m128d t[5];     // target
    __m128d d[5];     // per-sample increment while ramping
    __m128d z1, z2;   // TDF-II state
    int rampLeft;
    bool identity;    // current == identity in both lanes and not ramping

    void setIdentity()
    {
        const __m128d one = _mm_set1_pd(1.0), zero = _mm_setzero_pd();
        c[0] = t[0] = one;
        for (int k = 1; k < 5; ++k) c[k] = t[k] = zero;
        for (int k = 0; k < 5; ++k) d[k] = zero;
        z1 = z2 = zero;
        rampLeft = 0;
        identity = true;
    }

    bool targetIsIdentity() const
    {
        const __m128d one = _mm_set1_pd(1.0), zero = _mm_setzero_pd();
        return vecEqual(t[0], one) && vecEqual(t[1], zero) && vecEqual(t[2], zero) &&
               vecEqual(t[3], zero) && vecEqual(t[4], zero);
    }

    // Ramp linearly from the current coefficients to `target` over `samples`.
    void rampTo(const __m128d target[5], int samples)
    {
        const __m128d inv = _mm_set1_pd(1.0 / std::max(1, samples));
        for (int k = 0; k < 5; ++k) {
            t[k] = target[k];
            d[k] = _mm_mul_pd(_mm_sub_pd(t[k], c[k]), inv);
        }
        rampLeft = std::max(1, samples);
        identity = false;
    }

    void finishRamp()
    {
        for (int k = 0; k < 5; ++k) c[k] = t[k];
        rampLeft = 0;
        identity = targetIsIdentity();
        if (identity) {
            // An identity TDF-II section has (near) zero state already; clear
            // it so skipping the stage is exact and re-enabling starts clean.
            z1 = z2 = _mm_setzero_pd();
        }
    }

    void runSteady(double* x, int n)
    {
        const __m128d b0 = c[0], b1 = c[1], b2 = c[2], a1 = c[3], a2 = c[4];
        __m128d s1 = z1, s2 = z2;
        for (int i = 0; i < n; ++i) {
            const __m128d in = _mm_loadu_pd(x + 2 * i);
            const __m128d y = _mm_add_pd(_mm_mul_pd(b0, in), s1);
            s1 = _mm_add_pd(_mm_sub_pd(_mm_mul_pd(b1, in), _mm_mul_pd(a1, y)), s2);
            s2 = _mm_sub_pd(_mm_mul_pd(b2, in), _mm_mul_pd(a2, y));
            _mm_storeu_pd(x + 2 * i, y);
        }
        z1 = s1;
        z2 = s2;
    }

    void runRamp(double* x, int n)
    {
        __m128d b0 = c[0], b1 = c[1], b2 = c[2], a1 = c[3], a2 = c[4];
        const __m128d d0 = d[0], d1 = d[1], d2 = d[2], d3 = d[3], d4 = d[4];
        __m128d s1 = z1, s2 = z2;
        for (int i = 0; i < n; ++i) {
            b0 = _mm_add_pd(b0, d0);
            b1 = _mm_add_pd(b1, d1);
            b2 = _mm_add_pd(b2, d2);
            a1 = _mm_add_pd(a1, d3);
            a2 = _mm_add_pd(a2, d4);
            const __m128d in = _mm_loadu_pd(x + 2 * i);
            const __m128d y = _mm_add_pd(_mm_mul_pd(b0, in), s1);
            s1 = _mm_add_pd(_mm_sub_pd(_mm_mul_pd(b1, in), _mm_mul_pd(a1, y)), s2);
            s2 = _mm_sub_pd(_mm_mul_pd(b2, in), _mm_mul_pd(a2, y));
            _mm_storeu_pd(x + 2 * i, y);
        }
        c[0] = b0; c[1] = b1; c[2] = b2; c[3] = a1; c[4] = a2;
        z1 = s1;
        z2 = s2;
    }

    // Returns false if the state blew up (NaN/Inf input); state is then reset.
    bool process(double* x, int n)
    {
        if (rampLeft > 0) {
            const int m = std::min(n, rampLeft);
            runRamp(x, m);
            rampLeft -= m;
            if (rampLeft == 0)
                finishRamp();
            x += 2 * m;
            n -= m;
        }
        if (n > 0 && !identity)
            runSteady(x, n);

        // x - x is NaN for both NaN and Inf.
        const __m128d chk = _mm_add_pd(_mm_sub_pd(z1, z1), _mm_sub_pd(z2, z2));
        if (_mm_movemask_pd(_mm_cmpunord_pd(chk, chk)) != 0) {
            z1 = z2 = _mm_setzero_pd();
            return false;
        }
        return true;
    }
};

Equalizer::Equalizer() = default;
Equalizer::~Equalizer() = default;

bool Equalizer::prepare(double sampleRate, int numChannels, double rampMs)
{
    if (!(sampleRate >= 8000.0 && sampleRate <= 1536000.0) ||
        numChannels < 1 || numChannels > kMaxChannels)
        return false;

    sampleRate_ = sampleRate;
    numChannels_ = numChannels;
    numPairs_ = (numChannels + 1) / 2;
    rampSamples_ = std::max(1, static_cast<int>(std::lround(rampMs * 0.001 * sampleRate)));

    stages_.resize(static_cast<size_t>(numPairs_) * kMaxBands);
    for (Stage& s : stages_)
        s.setIdentity();
    buffer_.assign(static_cast<size_t>(numPairs_) * kBlock * 2, 0.0);
    scState_.assign(static_cast<size_t>(kMaxBands) * numChannels_, ScChannel{});
    dynCoefs_.assign(static_cast<size_t>(kMaxBands) * kMaxSub, BiquadCoeffs{});
    dynRetarget_.assign(static_cast<size_t>(kMaxBands) * kMaxSub, 0);
    for (Dyn& d : dyn_)
        d = Dyn{};

    // Limiter release: ~50 ms time constant.
    limiterRelease_ = std::exp(-1.0 / (0.050 * sampleRate));
    limiterGain_ = 1.0;
    limiterMinGain_ = 1.0;

    prepared_ = true;
    configureDynamics();
    applyTargets(true);
    return true;
}

void Equalizer::setSettings(const EqSettings& settings, bool immediate)
{
    settings_ = settings;
    settings_.numBands = std::min<uint32_t>(settings_.numBands, kMaxBands);
    if (prepared_) {
        configureDynamics();
        applyTargets(immediate);
    }
}

void Equalizer::configureDynamics()
{
    const EqSettings& s = settings_;
    anyDynamic_ = false;
    const double blockSec = kDynBlock / sampleRate_;
    for (int b = 0; b < kMaxBands; ++b) {
        Dyn& d = dyn_[b];
        const bool active = !s.bypass && b < static_cast<int>(s.numBands) && isDynamic(s.bands[b]);
        if (!active) {
            if (d.active) { // switched off: forget the state, the static ramp takes over
                d = Dyn{};
                std::fill_n(scState_.begin() + static_cast<ptrdiff_t>(b) * numChannels_, numChannels_, ScChannel{});
            }
            continue;
        }
        const Band& band = s.bands[b];
        // Sidechain: what this band "hears". Peaks: a band-pass at the band;
        // shelves: everything below / above the corner.
        Band sc;
        sc.enabled = 1;
        sc.frequencyHz = band.frequencyHz;
        if (band.type == FilterType::Peaking) {
            sc.type = FilterType::BandPass;
            sc.q = std::clamp(band.q, 0.5, 10.0);
        } else {
            sc.type = band.type == FilterType::LowShelf ? FilterType::LowPass : FilterType::HighPass;
            sc.q = 0.70710678118654752;
        }
        const BiquadCoeffs c = designBiquad(sc, sampleRate_);
        d.scB0 = c.b0;
        d.scB1 = c.b1;
        d.scB2 = c.b2;
        d.scA1 = c.a1;
        d.scA2 = c.a2;
        // Level detector: RMS over ~2 periods of the band frequency (2..20 ms),
        // so low bands don't ripple and high bands stay fast enough to de-ess.
        const double rmsSec = std::clamp(2.0 / std::max(band.frequencyHz, 1.0), 0.002, 0.020);
        d.powerCoef = std::exp(-blockSec / rmsSec);
        const double att = std::clamp(std::isfinite(band.attackMs) ? band.attackMs : 5.0, 0.1, 1000.0) * 1e-3;
        const double rel = std::clamp(std::isfinite(band.releaseMs) ? band.releaseMs : 120.0, 1.0, 5000.0) * 1e-3;
        d.attCoef = std::exp(-blockSec / att);
        d.relCoef = std::exp(-blockSec / rel);
        d.active = true;
        anyDynamic_ = true;
    }
}

double Equalizer::dynamicGainDb(int band) const
{
    return band >= 0 && band < kMaxBands && dyn_[band].active ? dyn_[band].change : 0.0;
}

// Runs every dynamic band's sidechain over the block's input and decides, per
// kDynBlock sub-block, the gain change and whether to retarget the filter.
void Equalizer::runSidechains(const float* in, int frames)
{
    const int ch = numChannels_;
    const int subs = (frames + kDynBlock - 1) / kDynBlock;
    for (int b = 0; b < kMaxBands; ++b) {
        Dyn& d = dyn_[b];
        if (!d.active)
            continue;
        const Band& band = settings_.bands[b];
        ScChannel* sc = &scState_[static_cast<size_t>(b) * ch];
        for (int sb = 0; sb < subs; ++sb) {
            const int off = sb * kDynBlock;
            const int len = std::min(kDynBlock, frames - off);
            double maxEnv = 0.0; // linked: loudest channel decides
            for (int c = 0; c < ch; ++c) {
                if (!(band.channelMask & (1u << c)))
                    continue;
                ScChannel& s = sc[c];
                double z1 = s.z1, z2 = s.z2, sum = 0.0;
                for (int f = 0; f < len; ++f) {
                    const double x = in[static_cast<size_t>(off + f) * ch + c];
                    const double y = d.scB0 * x + z1;
                    z1 = d.scB1 * x - d.scA1 * y + z2;
                    z2 = d.scB2 * x - d.scA2 * y;
                    // Envelope^2 = I^2 + Q^2 of the analytic signal: equals A^2
                    // for a sine of amplitude A, with almost no ripple.
                    const double i = s.delay;
                    s.delay = allpassChain(y, s.ap[0], kHilbertA[0]);
                    const double q = allpassChain(y, s.ap[1], kHilbertA[1]);
                    sum += i * i + q * q;
                }
                if (!std::isfinite(z1) || !std::isfinite(z2) || !std::isfinite(sum)) {
                    s = ScChannel{}; // bad input: restart this detector
                    z1 = z2 = sum = 0.0;
                }
                s.z1 = z1;
                s.z2 = z2;
                maxEnv = std::max(maxEnv, sum / len);
            }
            d.power = maxEnv + (d.power - maxEnv) * d.powerCoef;
            // Sine-referenced dBFS: a sine of amplitude A reads 20*log10(A).
            const double levelDb = 10.0 * std::log10(d.power + 1e-20);
            const double target = dynamicTarget(band, levelDb);
            const double coef = std::fabs(target) > std::fabs(d.change) ? d.attCoef : d.relCoef;
            d.change = target + (d.change - target) * coef;

            const size_t slot = static_cast<size_t>(b) * kMaxSub + sb;
            if (std::fabs(d.change - d.applied) > kRetargetDb) {
                dynCoefs_[slot] = designBiquad(withGainChange(band, d.change), sampleRate_);
                dynRetarget_[slot] = 1;
                d.applied = d.change;
            } else {
                dynRetarget_[slot] = 0;
            }
        }
    }
}

void Equalizer::applyTargets(bool immediate)
{
    const EqSettings& s = settings_;
    const BiquadCoeffs identity{};

    for (int b = 0; b < kMaxBands; ++b) {
        const bool active = !s.bypass && b < static_cast<int>(s.numBands);
        BiquadCoeffs designed = identity;
        if (active && dyn_[b].active) {
            // Keep the dynamic band where its dynamics currently have it.
            designed = designBiquad(withGainChange(s.bands[b], dyn_[b].change), sampleRate_);
            dyn_[b].applied = dyn_[b].change;
        } else if (active) {
            designed = designBiquad(s.bands[b], sampleRate_);
        }
        const uint32_t mask = active ? s.bands[b].channelMask : 0u;

        for (int p = 0; p < numPairs_; ++p) {
            const int ch0 = 2 * p, ch1 = 2 * p + 1;
            const BiquadCoeffs& l0 = (mask & (1u << ch0)) ? designed : identity;
            const BiquadCoeffs& l1 = (ch1 < numChannels_ && (mask & (1u << ch1))) ? designed : identity;

            __m128d t[5];
            t[0] = _mm_set_pd(l1.b0, l0.b0);
            t[1] = _mm_set_pd(l1.b1, l0.b1);
            t[2] = _mm_set_pd(l1.b2, l0.b2);
            t[3] = _mm_set_pd(l1.a1, l0.a1);
            t[4] = _mm_set_pd(l1.a2, l0.a2);

            Stage& st = stages_[static_cast<size_t>(p) * kMaxBands + b];

            bool same = true;
            for (int k = 0; k < 5; ++k)
                same = same && vecEqual(t[k], st.t[k]);

            if (same && !immediate)
                continue; // already heading there (or there)

            for (int k = 0; k < 5; ++k)
                st.t[k] = t[k];

            if (immediate) {
                st.finishRamp();
            } else {
                const __m128d inv = _mm_set1_pd(1.0 / rampSamples_);
                for (int k = 0; k < 5; ++k)
                    st.d[k] = _mm_mul_pd(_mm_sub_pd(st.t[k], st.c[k]), inv);
                st.rampLeft = rampSamples_;
                st.identity = false;
            }
        }
    }

    // Preamp
    const double preDb = std::clamp(std::isfinite(s.preampDb) ? s.preampDb : 0.0, -60.0, 30.0);
    gainTarget_ = s.bypass ? 1.0 : (preDb == 0.0 ? 1.0 : std::pow(10.0, preDb / 20.0));
    if (immediate || gainTarget_ == gain_) {
        gain_ = gainTarget_;
        gainRampLeft_ = 0;
    } else {
        gainDelta_ = (gainTarget_ - gain_) / rampSamples_;
        gainRampLeft_ = rampSamples_;
    }

    // Limiter
    const double ceilDb = std::clamp(std::isfinite(s.limiterCeilingDb) ? s.limiterCeilingDb : 0.0, -30.0, 0.0);
    limiterCeiling_ = ceilDb == 0.0 ? 1.0 : std::pow(10.0, ceilDb / 20.0);
    if (immediate)
        limiterGain_ = 1.0;
}

bool Equalizer::isTransparent() const
{
    if (gain_ != 1.0 || gainRampLeft_ != 0 || limiterGain_ != 1.0)
        return false;
    for (const Stage& s : stages_)
        if (!s.identity)
            return false;
    return true;
}

double Equalizer::takeLimiterReductionDb()
{
    const double g = limiterMinGain_;
    limiterMinGain_ = limiterGain_;
    return 20.0 * std::log10(std::max(g, 1e-10));
}

void Equalizer::reset()
{
    for (Stage& s : stages_) {
        s.finishRamp();
        s.z1 = s.z2 = _mm_setzero_pd();
    }
    gain_ = gainTarget_;
    gainRampLeft_ = 0;
    limiterGain_ = 1.0;
    limiterMinGain_ = 1.0;
    std::fill(scState_.begin(), scState_.end(), ScChannel{});
    for (Dyn& d : dyn_) {
        d.power = 0.0;
        d.change = 0.0;
    }
    if (prepared_)
        applyTargets(true); // dynamic bands back at their static gain
}

void Equalizer::process(const float* in, float* out, int frames)
{
    if (frames <= 0)
        return;
    if (!prepared_) {
        if (in != out)
            std::memmove(out, in, sizeof(float) * static_cast<size_t>(frames));
        return;
    }

    FtzDazGuard ftz;
    const int ch = numChannels_;
    while (frames > 0) {
        const int n = std::min(frames, kBlock);
        processBlock(in, out, n);
        in += static_cast<size_t>(n) * ch;
        out += static_cast<size_t>(n) * ch;
        frames -= n;
    }
}

void Equalizer::processBlock(const float* in, float* out, int n)
{
    const int ch = numChannels_;
    const size_t count = static_cast<size_t>(n) * ch;
    const bool limiterOn = settings_.limiterEnabled != 0;

    // Fast path: nothing to do -> bit-exact copy. Dynamic bands must keep
    // listening even while they're at 0 dB, so they disable it.
    if (!anyDynamic_ && isTransparent()) {
        bool needLimit = false;
        if (limiterOn) {
            const float ceil = static_cast<float>(limiterCeiling_);
            for (size_t i = 0; i < count && !needLimit; ++i)
                needLimit = std::fabs(in[i]) > ceil;
        }
        if (!needLimit) {
            if (in != out)
                std::memmove(out, in, sizeof(float) * count);
            return;
        }
    }

    // Deinterleave into channel-pair double buffers.
    double* buf = buffer_.data();
    constexpr size_t kPairStride = static_cast<size_t>(kBlock) * 2;
    for (int f = 0; f < n; ++f) {
        const float* frame = in + static_cast<size_t>(f) * ch;
        for (int c = 0; c < ch; ++c)
            buf[(c >> 1) * kPairStride + 2 * f + (c & 1)] = frame[c];
    }

    // Dynamics: measure each dynamic band on the (unprocessed) input first.
    if (anyDynamic_)
        runSidechains(in, n);

    // Filter cascade.
    const BiquadCoeffs identity{};
    for (int p = 0; p < numPairs_; ++p) {
        double* x = buf + p * kPairStride;
        Stage* stages = &stages_[static_cast<size_t>(p) * kMaxBands];
        for (int b = 0; b < kMaxBands; ++b) {
            Stage& s = stages[b];
            if (dyn_[b].active) {
                // Sub-block by sub-block, ramping to the coefficients the
                // dynamics asked for.
                const uint32_t mask = settings_.bands[b].channelMask;
                const int ch0 = 2 * p, ch1 = 2 * p + 1;
                for (int off = 0, sb = 0; off < n; off += kDynBlock, ++sb) {
                    const int len = std::min(kDynBlock, n - off);
                    const size_t slot = static_cast<size_t>(b) * kMaxSub + sb;
                    if (dynRetarget_[slot]) {
                        const BiquadCoeffs& l0 = (mask & (1u << ch0)) ? dynCoefs_[slot] : identity;
                        const BiquadCoeffs& l1 =
                            (ch1 < numChannels_ && (mask & (1u << ch1))) ? dynCoefs_[slot] : identity;
                        const __m128d target[5] = {_mm_set_pd(l1.b0, l0.b0), _mm_set_pd(l1.b1, l0.b1),
                                                   _mm_set_pd(l1.b2, l0.b2), _mm_set_pd(l1.a1, l0.a1),
                                                   _mm_set_pd(l1.a2, l0.a2)};
                        s.rampTo(target, len);
                    }
                    if (s.identity)
                        continue;
                    if (!s.process(x + 2 * off, len))
                        std::fill(x + 2 * off, x + 2 * (off + len), 0.0);
                }
                continue;
            }
            if (s.identity)
                continue;
            if (!s.process(x, n)) {
                // Bad input poisoned the filter; silence this block for the pair.
                std::fill(x, x + 2 * n, 0.0);
            }
        }
    }

    // Preamp, limiter, re-interleave.
    for (int f = 0; f < n; ++f) {
        if (gainRampLeft_ > 0) {
            gain_ += gainDelta_;
            if (--gainRampLeft_ == 0)
                gain_ = gainTarget_;
        }
        double g = gain_;

        if (limiterOn) {
            double peak = 0.0;
            for (int c = 0; c < ch; ++c)
                peak = std::max(peak, std::fabs(buf[(c >> 1) * kPairStride + 2 * f + (c & 1)] * g));

            const double target = peak > limiterCeiling_ ? limiterCeiling_ / peak : 1.0;
            if (target < limiterGain_) {
                limiterGain_ = target; // instant attack: never exceeds ceiling
            } else if (limiterGain_ < 1.0) {
                limiterGain_ = target + (limiterGain_ - target) * limiterRelease_;
                if (target == 1.0 && limiterGain_ > 1.0 - 1e-6)
                    limiterGain_ = 1.0;
            }
            limiterMinGain_ = std::min(limiterMinGain_, limiterGain_);
            g *= limiterGain_;
        } else {
            limiterGain_ = 1.0;
        }

        float* frame = out + static_cast<size_t>(f) * ch;
        for (int c = 0; c < ch; ++c)
            frame[c] = static_cast<float>(buf[(c >> 1) * kPairStride + 2 * f + (c & 1)] * g);
    }
}

} // namespace eqdsp
