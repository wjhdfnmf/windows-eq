// Self-contained unit tests for the eqdsp library (no external framework).

#include "eqdsp/AdaptiveResampler.h"
#include "eqdsp/BiquadDesign.h"
#include "eqdsp/Equalizer.h"
#include "eqdsp/PresetParser.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace eqdsp;

// ---------------------------------------------------------------- framework
namespace {

struct TestCase { const char* name; void (*fn)(); };
std::vector<TestCase>& registry() { static std::vector<TestCase> r; return r; }
struct Registrar { Registrar(const char* n, void (*f)()) { registry().push_back({n, f}); } };

int g_failures = 0;
bool g_currentFailed = false;

void fail(const char* file, int line, const std::string& msg)
{
    std::printf("    FAIL %s:%d: %s\n", file, line, msg.c_str());
    ++g_failures;
    g_currentFailed = true;
}

} // namespace

#define TEST(name) \
    static void name(); \
    static Registrar reg_##name(#name, name); \
    static void name()

#define CHECK(cond) \
    do { if (!(cond)) fail(__FILE__, __LINE__, #cond); } while (0)

#define CHECK_NEAR(a, b, tol) \
    do { \
        const double va_ = (a), vb_ = (b), vt_ = (tol); \
        if (!(std::fabs(va_ - vb_) <= vt_)) { \
            char buf_[256]; \
            std::snprintf(buf_, sizeof buf_, "%s = %.9g, expected %.9g (tol %.3g)", #a, va_, vb_, vt_); \
            fail(__FILE__, __LINE__, buf_); \
        } \
    } while (0)

// ---------------------------------------------------------------- helpers
namespace {

constexpr double kPi = 3.14159265358979323846;

Band makeBand(FilterType t, double f, double g, double q, uint32_t mask = kAllChannels)
{
    Band b;
    b.type = t;
    b.frequencyHz = f;
    b.gainDb = g;
    b.q = q;
    b.channelMask = mask;
    return b;
}

EqSettings makeSettings(std::initializer_list<Band> bands, double preampDb = 0.0)
{
    EqSettings s;
    s.numBands = 0;
    for (const Band& b : bands)
        s.bands[s.numBands++] = b;
    s.preampDb = preampDb;
    return s;
}

std::vector<float> sine(double freq, double fs, int frames, int channels, double amp)
{
    std::vector<float> v(static_cast<size_t>(frames) * channels);
    for (int i = 0; i < frames; ++i) {
        const float x = static_cast<float>(amp * std::sin(2.0 * kPi * freq * i / fs));
        for (int c = 0; c < channels; ++c)
            v[static_cast<size_t>(i) * channels + c] = x;
    }
    return v;
}

std::vector<float> noise(int samples, double amp, unsigned seed = 1)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> d(static_cast<float>(-amp), static_cast<float>(amp));
    std::vector<float> v(static_cast<size_t>(samples));
    for (float& x : v) x = d(rng);
    return v;
}

double rmsChannel(const std::vector<float>& v, int channels, int ch, int start, int count)
{
    double acc = 0.0;
    for (int i = start; i < start + count; ++i) {
        const double x = v[static_cast<size_t>(i) * channels + ch];
        acc += x * x;
    }
    return std::sqrt(acc / count);
}

// Run audio through the EQ in irregular chunk sizes, like a real audio engine.
void processChunked(Equalizer& eq, std::vector<float>& audio, int channels)
{
    const int frames = static_cast<int>(audio.size()) / channels;
    const int chunks[] = {480, 1, 37, 1024, 441, 256, 2000, 3};
    int pos = 0, k = 0;
    while (pos < frames) {
        const int n = std::min(chunks[k++ % 8], frames - pos);
        float* p = audio.data() + static_cast<size_t>(pos) * channels;
        eq.process(p, p, n);
        pos += n;
    }
}

// Measure gain of the processor at `freq` with a 1 s window (integer cycles
// for integer Hz at 48 kHz) after 0.5 s settling, in dB.
double measuredGainDb(const EqSettings& s, double freq, int channels = 2, int ch = 0, double fs = 48000.0)
{
    Equalizer eq;
    eq.prepare(fs, channels);
    eq.setSettings(s, true);
    const int settle = static_cast<int>(fs / 2), window = static_cast<int>(fs);
    std::vector<float> in = sine(freq, fs, settle + window, channels, 0.05);
    std::vector<float> out = in;
    processChunked(eq, out, channels);
    return 20.0 * std::log10(rmsChannel(out, channels, ch, settle, window) /
                             rmsChannel(in, channels, ch, settle, window));
}

} // namespace

// ---------------------------------------------------------------- design tests

TEST(peaking_hits_gain_at_center)
{
    for (double fs : {44100.0, 48000.0, 96000.0, 192000.0}) {
        for (double g : {-12.0, -3.0, 6.0, 15.0}) {
            const BiquadCoeffs c = designBiquad(makeBand(FilterType::Peaking, 1000, g, 1.41), fs);
            CHECK_NEAR(biquadMagnitudeDb(c, 1000, fs), g, 1e-9);
            CHECK_NEAR(biquadMagnitudeDb(c, 5.0, fs), 0.0, 1e-3);
        }
    }
}

TEST(shelves_reach_gain_at_extremes)
{
    const double fs = 48000;
    const BiquadCoeffs ls = designBiquad(makeBand(FilterType::LowShelf, 200, 8, 0.707), fs);
    CHECK_NEAR(biquadMagnitudeDb(ls, 1.0, fs), 8.0, 1e-3);
    CHECK_NEAR(biquadMagnitudeDb(ls, 23000, fs), 0.0, 1e-2);
    CHECK_NEAR(biquadMagnitudeDb(ls, 200, fs), 4.0, 1e-6); // half gain at corner

    const BiquadCoeffs hs = designBiquad(makeBand(FilterType::HighShelf, 4000, -6, 0.707), fs);
    CHECK_NEAR(biquadMagnitudeDb(hs, 23999, fs), -6.0, 1e-2);
    CHECK_NEAR(biquadMagnitudeDb(hs, 1.0, fs), 0.0, 1e-6);
    CHECK_NEAR(biquadMagnitudeDb(hs, 4000, fs), -3.0, 1e-6);
}

TEST(pass_filters_minus_3db_at_cutoff)
{
    const double fs = 48000;
    const double butterQ = 0.70710678118654752;
    const BiquadCoeffs lp = designBiquad(makeBand(FilterType::LowPass, 2000, 0, butterQ), fs);
    const BiquadCoeffs hp = designBiquad(makeBand(FilterType::HighPass, 2000, 0, butterQ), fs);
    const double minus3 = 20.0 * std::log10(butterQ);
    CHECK_NEAR(biquadMagnitudeDb(lp, 2000, fs), minus3, 1e-9);
    CHECK_NEAR(biquadMagnitudeDb(hp, 2000, fs), minus3, 1e-9);
    CHECK_NEAR(biquadMagnitudeDb(lp, 1, fs), 0.0, 1e-6);
    CHECK_NEAR(biquadMagnitudeDb(hp, 23999, fs), 0.0, 1e-3);
    CHECK(biquadMagnitudeDb(lp, 20000, fs) < -30.0);
    CHECK(biquadMagnitudeDb(hp, 100, fs) < -50.0);
}

TEST(bandpass_notch_allpass)
{
    const double fs = 48000;
    const BiquadCoeffs bp = designBiquad(makeBand(FilterType::BandPass, 3000, 0, 2), fs);
    CHECK_NEAR(biquadMagnitudeDb(bp, 3000, fs), 0.0, 1e-9);
    const BiquadCoeffs n = designBiquad(makeBand(FilterType::Notch, 3000, 0, 2), fs);
    CHECK(biquadMagnitudeDb(n, 3000, fs) < -200.0);
    const BiquadCoeffs ap = designBiquad(makeBand(FilterType::AllPass, 3000, 0, 2), fs);
    for (double f : {20.0, 300.0, 3000.0, 15000.0})
        CHECK_NEAR(biquadMagnitudeDb(ap, f, fs), 0.0, 1e-9);
}

TEST(low_frequency_precision_at_high_rate)
{
    // 10 Hz highpass at 384 kHz: 1 - cos(w0) is tiny; must still be accurate.
    const double fs = 384000;
    const BiquadCoeffs hp = designBiquad(makeBand(FilterType::HighPass, 10, 0, 0.70710678118654752), fs);
    CHECK_NEAR(biquadMagnitudeDb(hp, 10, fs), 20.0 * std::log10(0.70710678118654752), 1e-6);
}

TEST(identity_cases)
{
    CHECK(designBiquad(makeBand(FilterType::Peaking, 1000, 0, 1), 48000).isIdentity());
    CHECK(designBiquad(makeBand(FilterType::LowShelf, 100, 0, 1), 48000).isIdentity());
    Band off = makeBand(FilterType::Peaking, 1000, 6, 1);
    off.enabled = 0;
    CHECK(designBiquad(off, 48000).isIdentity());
    CHECK(designBiquad(makeBand(FilterType::Peaking, std::numeric_limits<double>::quiet_NaN(), 6, 1), 48000).isIdentity());
    CHECK(designBiquad(makeBand(FilterType::Peaking, 1000, 6, 1), 0).isIdentity());
    // Out-of-range parameters clamp to a valid, stable filter.
    const BiquadCoeffs c = designBiquad(makeBand(FilterType::Peaking, 1e9, 6, 1e-9), 48000);
    CHECK(!c.isIdentity());
    CHECK(std::fabs(c.a2) < 1.0 && std::fabs(c.a1) < 1.0 + c.a2);
}

TEST(auto_preamp_cancels_max_boost)
{
    EqSettings s = makeSettings({makeBand(FilterType::Peaking, 1000, 6, 1),
                                 makeBand(FilterType::Peaking, 1100, 3, 1),
                                 makeBand(FilterType::HighShelf, 8000, -4, 0.7)});
    const double pre = autoPreampDb(s, 2, 48000);
    CHECK(pre < -8.0 && pre > -9.5);
    s.preampDb = pre;
    for (int i = 0; i < 400; ++i) {
        const double f = 20.0 * std::pow(1000.0, i / 399.0);
        CHECK(responseDb(s, 0, f, 48000) <= 1e-3);
    }
    EqSettings cut = makeSettings({makeBand(FilterType::Peaking, 1000, -6, 1)});
    CHECK_NEAR(autoPreampDb(cut, 2, 48000), 0.0, 1e-12);
}

// ---------------------------------------------------------------- processing tests

TEST(null_test_flat_is_bit_exact)
{
    // 0 dB bands, 0 dB preamp, limiter on, multiple formats -> exact passthrough.
    for (int channels : {1, 2, 3, 6, 8}) {
        Equalizer eq;
        CHECK(eq.prepare(48000, channels));
        eq.setSettings(makeSettings({makeBand(FilterType::Peaking, 100, 0, 1),
                                     makeBand(FilterType::LowShelf, 50, 0, 0.7)}), true);
        const std::vector<float> in = noise(48000 * channels, 0.99);
        std::vector<float> out = in;
        processChunked(eq, out, channels);
        CHECK(std::memcmp(in.data(), out.data(), in.size() * sizeof(float)) == 0);
    }
}

TEST(bypass_is_bit_exact_after_ramp)
{
    Equalizer eq;
    eq.prepare(48000, 2);
    EqSettings s = makeSettings({makeBand(FilterType::Peaking, 1000, 9, 1)}, -3.0);
    eq.setSettings(s, true);
    std::vector<float> warm = noise(48000 * 2, 0.5, 7);
    processChunked(eq, warm, 2);

    s.bypass = 1;
    eq.setSettings(s); // ramped
    std::vector<float> ramp = noise(2400 * 2, 0.5, 8); // 50 ms > 20 ms ramp
    processChunked(eq, ramp, 2);
    CHECK(eq.isTransparent());

    const std::vector<float> in = noise(4800 * 2, 0.5, 9);
    std::vector<float> out = in;
    processChunked(eq, out, 2);
    CHECK(std::memcmp(in.data(), out.data(), in.size() * sizeof(float)) == 0);
}

TEST(measured_gain_matches_design)
{
    const EqSettings s = makeSettings({makeBand(FilterType::Peaking, 1000, 6, 1.0),
                                       makeBand(FilterType::LowShelf, 105, 5, 0.7),
                                       makeBand(FilterType::HighShelf, 9000, -4, 0.7),
                                       makeBand(FilterType::Peaking, 3500, -8, 4.0)},
                                      -2.0);
    for (double f : {50.0, 100.0, 440.0, 1000.0, 3500.0, 7000.0, 15000.0})
        CHECK_NEAR(measuredGainDb(s, f), responseDb(s, 0, f, 48000), 0.005);
}

TEST(channel_mask_isolates_channels)
{
    // Band only on the left channel: right must be bit-exact.
    const EqSettings s = makeSettings({makeBand(FilterType::Peaking, 1000, 10, 1, 0x1)});
    Equalizer eq;
    eq.prepare(48000, 2);
    eq.setSettings(s, true);
    const std::vector<float> in = noise(48000 * 2, 0.3, 3);
    std::vector<float> out = in;
    processChunked(eq, out, 2);
    bool rightExact = true, leftChanged = false;
    for (size_t i = 0; i < in.size(); i += 2) {
        rightExact = rightExact && in[i + 1] == out[i + 1];
        leftChanged = leftChanged || in[i] != out[i];
    }
    CHECK(rightExact);
    CHECK(leftChanged);
    CHECK_NEAR(measuredGainDb(s, 1000, 2, 0), 10.0, 0.005);
    CHECK_NEAR(measuredGainDb(s, 1000, 2, 1), 0.0, 1e-9);
}

TEST(odd_channel_count_last_channel)
{
    // Band only on channel 4 of 5 (the unpaired one).
    const EqSettings s = makeSettings({makeBand(FilterType::Peaking, 1000, -7, 1, 1u << 4)});
    CHECK_NEAR(measuredGainDb(s, 1000, 5, 4), -7.0, 0.005);
    CHECK_NEAR(measuredGainDb(s, 1000, 5, 3), 0.0, 1e-9);
}

TEST(in_place_matches_out_of_place)
{
    const EqSettings s = makeSettings({makeBand(FilterType::Peaking, 700, 5, 2),
                                       makeBand(FilterType::HighPass, 30, 0, 0.7)});
    Equalizer a, b;
    a.prepare(44100, 2);
    b.prepare(44100, 2);
    a.setSettings(s, true);
    b.setSettings(s, true);
    const std::vector<float> in = noise(44100 * 2, 0.4, 11);
    std::vector<float> inPlace = in, outOfPlace(in.size());
    a.process(inPlace.data(), inPlace.data(), 44100);
    b.process(in.data(), outOfPlace.data(), 44100);
    CHECK(std::memcmp(inPlace.data(), outOfPlace.data(), in.size() * sizeof(float)) == 0);
}

namespace {

// Peak level (dBFS) of content above 8 kHz in channel 0 after `from`: a
// click / zipper is broadband, while the 300 Hz test tone is not.
double clickLevelDb(const std::vector<float>& audio, int from)
{
    Band hp = makeBand(FilterType::HighPass, 8000, 0, 0.70710678118654752);
    const BiquadCoeffs c = designBiquad(hp, 48000);
    double z[4][2] = {};
    double peak = 0.0;
    const int frames = static_cast<int>(audio.size() / 2);
    for (int i = 0; i < frames; ++i) {
        double x = audio[static_cast<size_t>(i) * 2];
        for (auto& s : z) { // 4 cascaded sections = 8th-order highpass
            const double y = c.b0 * x + s[0];
            s[0] = c.b1 * x - c.a1 * y + s[1];
            s[1] = c.b2 * x - c.a2 * y;
            x = y;
        }
        if (i >= from)
            peak = std::max(peak, std::fabs(x));
    }
    return 20.0 * std::log10(std::max(peak, 1e-12));
}

} // namespace

TEST(ramp_is_smooth_and_converges)
{
    // Abruptly switch on a big, narrow boost/cut while a tone plays. A ramped
    // change must produce far less broadband (click) energy than an instant
    // one, and must end up identical to an EQ that started with those settings.
    const double fs = 48000, f = 300, amp = 0.3;
    const int frames = 48000, switchAt = 12000;
    const std::vector<float> in = sine(f, fs, frames, 2, amp);
    const EqSettings change = makeSettings({makeBand(FilterType::Peaking, f, -18, 4),
                                            makeBand(FilterType::HighShelf, 2000, 12, 0.7),
                                            makeBand(FilterType::LowShelf, 500, 12, 0.7)},
                                           -6.0);

    auto render = [&](bool immediate, Equalizer& eq) {
        eq.prepare(fs, 2);
        std::vector<float> a = in;
        eq.process(a.data(), a.data(), switchAt);
        eq.setSettings(change, immediate);
        eq.process(a.data() + switchAt * 2, a.data() + switchAt * 2, frames - switchAt);
        return a;
    };
    Equalizer e1, e2;
    const std::vector<float> ramped = render(false, e1);
    const std::vector<float> instant = render(true, e2);

    const double rampClick = clickLevelDb(ramped, switchAt);
    const double instantClick = clickLevelDb(instant, switchAt);
    std::printf("    click level above 8 kHz: ramped %.1f dBFS, instant %.1f dBFS\n",
                rampClick, instantClick);
    CHECK(rampClick < instantClick - 30.0);
    CHECK(rampClick < -90.0);

    Equalizer ref;
    ref.prepare(fs, 2);
    ref.setSettings(change, true);
    std::vector<float> b = in;
    ref.process(b.data(), b.data(), frames);
    const std::vector<float>& a = ramped;

    double maxDiff = 0.0;
    for (int i = frames - 4800; i < frames; ++i)
        maxDiff = std::max(maxDiff, static_cast<double>(std::fabs(a[i * 2] - b[i * 2])));
    CHECK(maxDiff < 1e-6);
}

TEST(preamp_gain_applies)
{
    const EqSettings s = makeSettings({}, -6.0);
    CHECK_NEAR(measuredGainDb(s, 1000), -6.0, 1e-6);
}

TEST(limiter_caps_peaks)
{
    Equalizer eq;
    eq.prepare(48000, 2);
    EqSettings s = makeSettings({makeBand(FilterType::Peaking, 1000, 12, 1)});
    s.limiterCeilingDb = -1.0;
    eq.setSettings(s, true);
    std::vector<float> audio = sine(1000, 48000, 48000, 2, 0.8); // would hit ~3.2
    processChunked(eq, audio, 2);
    const float ceil = static_cast<float>(std::pow(10.0, -1.0 / 20.0));
    float peak = 0;
    for (float x : audio) peak = std::max(peak, std::fabs(x));
    CHECK(peak <= ceil * 1.000001f);
    CHECK(peak > ceil * 0.9f);
    CHECK(eq.takeLimiterReductionDb() < -10.0);

    // Disabled limiter lets it through.
    s.limiterEnabled = 0;
    Equalizer raw;
    raw.prepare(48000, 2);
    raw.setSettings(s, true);
    std::vector<float> loud = sine(1000, 48000, 48000, 2, 0.8);
    processChunked(raw, loud, 2);
    peak = 0;
    for (float x : loud) peak = std::max(peak, std::fabs(x));
    CHECK(peak > 3.0f);
}

TEST(extreme_settings_stay_stable)
{
    // Very narrow, very low, very loud filters at high sample rates on noise.
    for (double fs : {44100.0, 192000.0, 384000.0}) {
        EqSettings s;
        s.limiterEnabled = 0;
        s.numBands = kMaxBands;
        for (int i = 0; i < kMaxBands; ++i)
            s.bands[i] = makeBand(i % 2 ? FilterType::Peaking : FilterType::LowShelf,
                                  10.0 * std::pow(1.25, i), (i % 3 - 1) * 20.0, 0.1 + i * 0.5);
        s.preampDb = autoPreampDb(s, 2, fs);
        Equalizer eq;
        eq.prepare(fs, 2);
        eq.setSettings(s, true);
        std::vector<float> audio = noise(static_cast<int>(fs) * 2, 0.5, 5);
        processChunked(eq, audio, 2);
        bool finite = true;
        for (float x : audio) finite = finite && std::isfinite(x);
        CHECK(finite);
    }
}

TEST(nan_input_recovers)
{
    Equalizer eq;
    eq.prepare(48000, 2);
    eq.setSettings(makeSettings({makeBand(FilterType::Peaking, 1000, 6, 1)}), true);
    std::vector<float> audio = noise(4800 * 2, 0.3, 2);
    audio[1000] = std::numeric_limits<float>::quiet_NaN();
    audio[1001] = std::numeric_limits<float>::infinity();
    eq.process(audio.data(), audio.data(), 4800);
    std::vector<float> after = noise(4800 * 2, 0.3, 4);
    eq.process(after.data(), after.data(), 4800);
    bool finite = true;
    for (float x : after) finite = finite && std::isfinite(x);
    CHECK(finite);
}

TEST(sample_rate_change_keeps_settings)
{
    const EqSettings s = makeSettings({makeBand(FilterType::Peaking, 1000, 6, 1)});
    Equalizer eq;
    eq.prepare(44100, 2);
    eq.setSettings(s, true);
    CHECK(eq.prepare(96000, 6));
    CHECK(eq.settings().numBands == 1);
    CHECK(!eq.isTransparent());
    CHECK(!eq.prepare(1000, 2));
    CHECK(!eq.prepare(48000, 0));
    CHECK(!eq.prepare(48000, 33));
}

TEST(denormal_tail_decays_to_zero)
{
    // After loud input followed by long silence, the IIR tail must flush to
    // exact zero (FTZ/DAZ) instead of lingering as slow denormals.
    Equalizer eq;
    eq.prepare(48000, 2);
    eq.setSettings(makeSettings({makeBand(FilterType::LowShelf, 30, 12, 0.5),
                                 makeBand(FilterType::Peaking, 60, 10, 8)}), true);
    std::vector<float> loud = noise(48000 * 2, 0.9, 6);
    eq.process(loud.data(), loud.data(), 48000);
    std::vector<float> silence(48000 * 20 * 2, 0.0f);
    eq.process(silence.data(), silence.data(), 48000 * 20);
    bool tiny = true;
    for (size_t i = silence.size() - 2000; i < silence.size(); ++i)
        tiny = tiny && std::fabs(silence[i]) < 1e-20f;
    CHECK(tiny);
}

// ---------------------------------------------------------------- resampler

namespace {

// Least-squares fit of a sine at known frequency (cycles/sample) to channel 0;
// returns signal-to-residual ratio in dB over [start, start+count).
double sineSnrDb(const std::vector<float>& y, int channels, int start, int count, double cyclesPerSample)
{
    double ss = 0, cc = 0, sc = 0, ys = 0, yc = 0;
    for (int n = start; n < start + count; ++n) {
        const double s = std::sin(2 * kPi * cyclesPerSample * n), c = std::cos(2 * kPi * cyclesPerSample * n);
        const double v = y[static_cast<size_t>(n) * channels];
        ss += s * s; cc += c * c; sc += s * c; ys += v * s; yc += v * c;
    }
    const double det = ss * cc - sc * sc;
    const double a = (ys * cc - yc * sc) / det, b = (yc * ss - ys * sc) / det;
    double sig = 0, res = 0;
    for (int n = start; n < start + count; ++n) {
        const double fit = a * std::sin(2 * kPi * cyclesPerSample * n) + b * std::cos(2 * kPi * cyclesPerSample * n);
        const double v = y[static_cast<size_t>(n) * channels];
        sig += fit * fit;
        res += (v - fit) * (v - fit);
    }
    return 10.0 * std::log10(sig / std::max(res, 1e-300));
}

// Resample a whole buffer through the pull API in irregular output chunks.
std::vector<float> resampleAll(AdaptiveResampler& rs, const std::vector<float>& in, int channels, int outFrames)
{
    std::vector<float> out(static_cast<size_t>(outFrames) * channels);
    size_t readPos = 0;
    const size_t inFrames = in.size() / channels;
    auto pull = [&](float* dst, int frames) {
        const int n = static_cast<int>(std::min<size_t>(frames, inFrames - readPos));
        std::memcpy(dst, in.data() + readPos * channels, sizeof(float) * static_cast<size_t>(n) * channels);
        readPos += static_cast<size_t>(n);
        return n;
    };
    const int chunks[] = {480, 441, 1, 512, 97, 1024};
    int done = 0, k = 0;
    while (done < outFrames) {
        const int n = std::min(chunks[k++ % 6], outFrames - done);
        rs.process(out.data() + static_cast<size_t>(done) * channels, n, pull);
        done += n;
    }
    return out;
}

} // namespace

TEST(resampler_sine_quality_across_ratios)
{
    struct Case { double inRate, outRate, corr, freq; };
    const Case cases[] = {
        {48000, 48000, 1.0, 1000},    {48000, 48000, 1.0001, 1000}, {48000, 48000, 0.9997, 15000},
        {44100, 48000, 1.0, 997},     {48000, 44100, 1.0, 997},     {96000, 48000, 1.0002, 5000},
        {48000, 96000, 1.0, 19000},
    };
    for (const Case& c : cases) {
        AdaptiveResampler rs;
        CHECK(rs.prepare(2, c.inRate, c.outRate, 1024));
        rs.setCorrection(c.corr);
        const int inFrames = static_cast<int>(c.inRate * 1.2);
        const std::vector<float> in = sine(c.freq, c.inRate, inFrames, 2, 0.5);
        const int outFrames = static_cast<int>(c.outRate * 1.0);
        const std::vector<float> out = resampleAll(rs, in, 2, outFrames);
        const double cps = c.freq / c.inRate * rs.step(); // output cycles per sample
        const double snr = sineSnrDb(out, 2, outFrames / 4, outFrames / 2, cps);
        std::printf("    %6g -> %6g Hz (x%.4f), %5g Hz tone: SNR %.1f dB\n", c.inRate, c.outRate, c.corr, c.freq, snr);
        CHECK(snr > 100.0);
    }
}

TEST(resampler_passband_flat_and_rejects_aliases)
{
    // 48k -> 48k: gain flat within 0.01 dB up to 19 kHz.
    for (double f : {20.0, 1000.0, 10000.0, 19000.0}) {
        AdaptiveResampler rs;
        rs.prepare(1, 48000, 48000, 512);
        const std::vector<float> in = sine(f, 48000, 60000, 1, 0.5);
        const std::vector<float> out = resampleAll(rs, in, 1, 48000);
        const double gain = 20 * std::log10(rmsChannel(out, 1, 0, 12000, 24000) / rmsChannel(in, 1, 0, 12000, 24000));
        CHECK_NEAR(gain, 0.0, 0.01);
    }
    // 96k -> 48k: a 30 kHz tone (above the output Nyquist) must be removed.
    AdaptiveResampler rs;
    rs.prepare(1, 96000, 48000, 512);
    const std::vector<float> in = sine(30000, 96000, 120000, 1, 0.5);
    const std::vector<float> out = resampleAll(rs, in, 1, 48000);
    const double level = 20 * std::log10(rmsChannel(out, 1, 0, 12000, 24000) / rmsChannel(in, 1, 0, 12000, 24000));
    CHECK(level < -90.0);
}

TEST(resampler_reports_underrun)
{
    AdaptiveResampler rs;
    rs.prepare(2, 48000, 48000, 480);
    std::vector<float> out(480 * 2);
    auto starve = [](float*, int) { return 0; };
    rs.process(out.data(), 480, starve);
    CHECK(rs.missingInputFrames() > 0);
    bool silent = true;
    for (float x : out) silent = silent && x == 0.0f;
    CHECK(silent);
}

TEST(drift_controller_locks_to_clock_difference)
{
    // Simulated capture clock 150 ppm fast, delivering 480-frame packets;
    // render pulls 441..480-frame periods. FIFO + resampler fill must settle
    // at the target and the correction must converge to ~+150 ppm.
    const double inRate = 48000, outRate = 48000, drift = 150e-6;
    const double target = 1200;
    AdaptiveResampler rs;
    rs.prepare(2, inRate, outRate, 1024);
    DriftController dc;
    dc.prepare(inRate, target);

    double fifo = 0;               // frames waiting in the FIFO (content irrelevant)
    double captureClock = 0, renderClock = 0;
    const double capturePeriod = 480 / (inRate * (1 + drift));
    std::vector<float> scratch(1024 * 2);
    long long underrunsAfterLock = 0;
    double maxErrLate = 0, ppmSum = 0, ppmMin = 1e9, ppmMax = -1e9;
    int ppmCount = 0;
    auto pull = [&](float* dst, int frames) {
        const int n = static_cast<int>(std::min<double>(frames, fifo));
        std::fill(dst, dst + static_cast<size_t>(n) * 2, 0.0f);
        fifo -= n;
        return n;
    };

    // Prefill to target, as the router does at start-up.
    fifo = target;
    const double total = 300.0; // seconds of virtual time
    int period = 0;
    while (renderClock < total) {
        const int frames = (period++ % 3 == 0) ? 441 : 480;
        const double dt = frames / outRate;
        renderClock += dt;
        while (captureClock + capturePeriod <= renderClock) {
            captureClock += capturePeriod;
            fifo += 480;
        }
        // Continuous fill estimate: add what has accrued since the last packet.
        const double accrued = std::min(480.0, (renderClock - captureClock) * inRate);
        const double fill = fifo + rs.bufferedInputFrames() + accrued;
        rs.setCorrection(dc.update(fill, dt));
        const long long before = rs.missingInputFrames();
        rs.process(scratch.data(), frames, pull);
        if (renderClock > 5.0)
            underrunsAfterLock += rs.missingInputFrames() - before;
        if (renderClock > 200.0) {
            maxErrLate = std::max(maxErrLate, std::fabs(dc.smoothedFill() - target));
            ppmSum += dc.ppm();
            ppmMin = std::min(ppmMin, dc.ppm());
            ppmMax = std::max(ppmMax, dc.ppm());
            ++ppmCount;
        }
    }
    const double ppmMean = ppmSum / ppmCount;
    std::printf("    settled: correction %+.1f ppm (range %+.1f..%+.1f) for clock drift %+.1f ppm; max fill error %.1f frames\n",
                ppmMean, ppmMin, ppmMax, drift * 1e6, maxErrLate);
    CHECK(underrunsAfterLock == 0);
    CHECK_NEAR(ppmMean, drift * 1e6, 2.0);
    CHECK(ppmMax - ppmMin < 20.0); // pitch modulation well under 0.05 cents
    CHECK(maxErrLate < 20.0);
}

// Controller-level plant: fill' = rate * (drift - u), plus output stalls that
// step the fill up by whole packets. Returns the worst dip below target after
// the stalls, and the mean correction over the last 60 s.
static void simulateStalls(double band, double drift, double& worstDip, double& ppmEnd)
{
    const double rate = 48000, target = 1400, dt = 0.01;
    DriftController dc;
    dc.prepare(rate, target, 15.0, 1.0, 5000.0, band);
    double fill = target;
    worstDip = 0;
    double ppmSum = 0;
    int n = 0;
    for (int i = 0; i < 300 * 100; ++i) {
        const double t = i * dt;
        if (i == 6000 || i == 6080 || i == 6400)
            fill += 480; // the output consumed nothing for one 10 ms period
        const double u = dc.update(fill, dt) - 1.0;
        fill += rate * dt * (drift - u);
        if (t > 60.0)
            worstDip = std::min(worstDip, fill - target);
        if (t > 240.0) {
            ppmSum += dc.ppm();
            ++n;
        }
    }
    ppmEnd = ppmSum / n;
}

TEST(drift_controller_band_limits_windup_from_output_stalls)
{
    double dipFree = 0, ppmFree = 0, dipBand = 0, ppmBand = 0;
    simulateStalls(0.0, 150e-6, dipFree, ppmFree);
    simulateStalls(480.0, 150e-6, dipBand, ppmBand);
    std::printf("    three 480-frame stalls: dip below target %.0f frames unclamped, %.0f with band; lock %+.1f / %+.1f ppm\n",
                dipFree, dipBand, ppmFree, ppmBand);
    CHECK(dipBand > 0.8 * dipFree); // dips are negative: band dip is >20% smaller
    CHECK(dipBand > -200.0);        // stays well inside the 4 ms (192-frame) margin + packet
    CHECK_NEAR(ppmBand, 150.0, 2.0);

    // A real clock offset inside the band must still lock fully.
    double dipBig = 0, ppmBig = 0;
    simulateStalls(480.0, -1000e-6, dipBig, ppmBig);
    CHECK_NEAR(ppmBig, -1000.0, 5.0);
}

TEST(drift_controller_keeps_rate_across_rebuffer)
{
    DriftController dc;
    dc.prepare(48000, 1200, 15.0, 1.0, 5000.0, 480.0);
    double fill = 1200;
    for (int i = 0; i < 200 * 100; ++i) {
        const double u = dc.update(fill, 0.01) - 1.0;
        fill += 480 * (300e-6 - u);
    }
    CHECK_NEAR(dc.ppm(), 300.0, 5.0);
    dc.reset(true);
    CHECK_NEAR(dc.ppm(), 300.0, 5.0);
    dc.reset();
    CHECK(dc.ppm() == 0.0);
}

// ---------------------------------------------------------------- preset parser

TEST(parses_autoeq_parametric_file)
{
    const char* text =
        "Preamp: -6.2 dB\n"
        "Filter 1: ON LSC Fc 105 Hz Gain 6.0 dB Q 0.70\n"
        "Filter 2: ON PK Fc 1520 Hz Gain -2.3 dB Q 1.41\n"
        "Filter 3: ON HSC Fc 10000 Hz Gain -4.1 dB Q 0.70\n"
        "Filter 4: OFF PK Fc 3000 Hz Gain 2 dB Q 2\n";
    EqSettings s;
    std::vector<std::string> w;
    CHECK(parseEqualizerApoConfig(text, s, w));
    CHECK(w.empty());
    CHECK_NEAR(s.preampDb, -6.2, 1e-12);
    CHECK(s.numBands == 4);
    CHECK(s.bands[0].type == FilterType::LowShelf);
    CHECK_NEAR(s.bands[0].frequencyHz, 105, 1e-12);
    CHECK_NEAR(s.bands[0].gainDb, 6.0, 1e-12);
    CHECK_NEAR(s.bands[0].q, 0.70, 1e-12);
    CHECK(s.bands[1].type == FilterType::Peaking);
    CHECK(s.bands[2].type == FilterType::HighShelf);
    CHECK(s.bands[3].enabled == 0);
}

TEST(parses_channels_bandwidth_and_reports_problems)
{
    const char* text =
        "\xEF\xBB\xBF# headphone correction\n"
        "Channel: L\n"
        "Filter: ON PK Fc 1000 Hz Gain 3 dB BW Oct 1.0\n"
        "Channel: R\n"
        "Filter: ON LP Fc 18000 Hz\n"
        "Channel: all\n"
        "Filter: ON NO Fc 60 Hz\n"            // no Q -> warning, default used
        "GraphicEQ: 20 0; 40 1\n"             // unsupported -> warning
        "Filter: ON XYZ Fc 1 Hz\n"            // bad type -> warning
        "Filter: ON PK Gain 3 dB Q 1\n"       // missing Fc -> skipped
        "Preamp: -3dB\n"                      // unit glued on
        "Preamp: -1 dB\n";                    // accumulates
    EqSettings s;
    std::vector<std::string> w;
    CHECK(parseEqualizerApoConfig(text, s, w));
    CHECK(s.numBands == 3);
    CHECK(s.bands[0].channelMask == 0x1);
    CHECK_NEAR(s.bands[0].q, std::sqrt(2.0) / (2.0 - 1.0), 1e-12); // BW 1 oct
    CHECK(s.bands[1].channelMask == 0x2);
    CHECK(s.bands[1].type == FilterType::LowPass);
    CHECK_NEAR(s.bands[1].q, 0.70710678118654752, 1e-12);
    CHECK(s.bands[2].channelMask == kAllChannels);
    CHECK_NEAR(s.preampDb, -4.0, 1e-12);
    CHECK(w.size() == 4);
}

TEST(preset_round_trips)
{
    EqSettings s = makeSettings({makeBand(FilterType::Peaking, 1234.5, -3.25, 2.5, 0x1),
                                 makeBand(FilterType::HighShelf, 8000, 4, 0.7),
                                 makeBand(FilterType::HighPass, 25, 0, 0.5, 0x2)},
                                -2.5);
    s.bands[1].enabled = 0;
    const std::string text = formatEqualizerApoConfig(s);
    EqSettings back;
    std::vector<std::string> w;
    CHECK(parseEqualizerApoConfig(text, back, w));
    CHECK(w.empty());
    CHECK(back.numBands == 3);
    CHECK_NEAR(back.preampDb, -2.5, 1e-9);
    for (int i = 0; i < 3; ++i) {
        CHECK(back.bands[i].type == s.bands[i].type);
        CHECK(back.bands[i].enabled == s.bands[i].enabled);
        CHECK(back.bands[i].channelMask == s.bands[i].channelMask);
        CHECK_NEAR(back.bands[i].frequencyHz, s.bands[i].frequencyHz, 1e-3);
        CHECK_NEAR(back.bands[i].q, s.bands[i].q, 1e-5);
        CHECK_NEAR(back.bands[i].gainDb, s.bands[i].gainDb, 1e-3);
    }
}

// ---------------------------------------------------------------- main

int main()
{
    int passed = 0;
    for (const TestCase& t : registry()) {
        g_currentFailed = false;
        t.fn();
        std::printf("%s %s\n", g_currentFailed ? "[FAIL]" : "[ ok ]", t.name);
        if (!g_currentFailed) ++passed;
    }
    std::printf("\n%d/%zu tests passed, %d failed checks\n", passed, registry().size(), g_failures);
    return g_failures == 0 ? 0 : 1;
}
