#include "Presets.h"

#include "eqdsp/BiquadDesign.h"
#include "eqdsp/Equalizer.h"
#include "eqdsp/PresetParser.h"

#include <cmath>
#include <initializer_list>
#include <set>

namespace gui {

namespace {

using eqdsp::FilterType;

struct Dyn {
    eqdsp::DynMode mode;
    double threshold, ratio, range, attackMs, releaseMs;
};

struct B {
    FilterType type;
    double f, g, q;
    bool dynamic = false;
    Dyn dyn{};
};

BuiltinPreset make(const wchar_t* name, std::initializer_list<B> bands)
{
    BuiltinPreset p;
    p.name = name;
    for (const B& b : bands) {
        eqdsp::Band& band = p.settings.bands[p.settings.numBands++];
        band.type = b.type;
        band.frequencyHz = b.f;
        band.gainDb = b.g;
        band.q = b.q;
        if (b.dynamic) {
            band.dynamic = 1;
            band.dynMode = b.dyn.mode;
            band.thresholdDb = b.dyn.threshold;
            band.ratio = b.dyn.ratio;
            band.rangeDb = b.dyn.range;
            band.attackMs = b.dyn.attackMs;
            band.releaseMs = b.dyn.releaseMs;
        }
    }
    // Round the anti-clip preamp down to 0.1 dB.
    p.settings.preampDb = std::floor(eqdsp::autoPreampDb(p.settings, 2, 48000.0) * 10.0) / 10.0;
    return p;
}

} // namespace

const std::vector<BuiltinPreset>& builtinPresets()
{
    static const std::vector<BuiltinPreset> presets = [] {
        std::vector<BuiltinPreset> v;
        v.push_back(make(L"Flat", {}));
        v.push_back(make(L"Bass Boost", {{FilterType::LowShelf, 110, 6.0, 0.71}}));
        v.push_back(make(L"Bass Boost (strong)", {{FilterType::LowShelf, 120, 10.0, 0.71}}));
        v.push_back(make(L"Treble Boost", {{FilterType::HighShelf, 6000, 5.0, 0.71}}));
        v.push_back(make(L"V-Shape", {{FilterType::LowShelf, 100, 5.0, 0.71},
                                      {FilterType::Peaking, 1500, -3.0, 0.8},
                                      {FilterType::HighShelf, 8000, 4.0, 0.71}}));
        v.push_back(make(L"Vocal Clarity", {{FilterType::Peaking, 250, -2.5, 1.0},
                                            {FilterType::Peaking, 3000, 3.0, 1.0},
                                            {FilterType::HighShelf, 10000, 1.5, 0.71}}));
        v.push_back(make(L"Loudness (low volume)", {{FilterType::LowShelf, 90, 7.0, 0.71},
                                                    {FilterType::HighShelf, 10000, 4.0, 0.71}}));
        v.push_back(make(L"Rumble Filter", {{FilterType::HighPass, 35, 0.0, 0.71}}));
        v.push_back(make(L"Telephone (test)", {{FilterType::HighPass, 400, 0.0, 0.9},
                                               {FilterType::LowPass, 3400, 0.0, 0.9},
                                               {FilterType::Peaking, 1800, 4.0, 1.2}}));
        // Dynamic presets.
        using eqdsp::DynMode;
        // Cuts sibilance ("s", "t") at 5-9 kHz only when it gets loud.
        v.push_back(make(L"De-esser (dynamic)",
                         {{FilterType::Peaking, 6500, 0.0, 2.0, true, {DynMode::Above, -32, 4, -8, 1, 60}}}));
        // Tames harsh upper mids and boomy bass only on loud passages.
        v.push_back(make(L"Tame harshness (dynamic)",
                         {{FilterType::Peaking, 3000, 0.0, 1.4, true, {DynMode::Above, -26, 3, -6, 5, 120}},
                          {FilterType::LowShelf, 120, 0.0, 0.71, true, {DynMode::Above, -18, 2, -4, 20, 250}}}));
        // Lifts bass and treble only while they're quiet, e.g. at low volume.
        v.push_back(make(L"Dynamic loudness",
                         {{FilterType::LowShelf, 100, 0.0, 0.71, true, {DynMode::Below, -32, 2, 7, 20, 300}},
                          {FilterType::HighShelf, 9000, 0.0, 0.71, true, {DynMode::Below, -40, 2, 4, 10, 300}}}));

        // ---- Voice and video
        // Rumble/boom off, mud cut, 2-4 kHz intelligibility lift, sibilance tamed when loud.
        v.push_back(make(L"Speech / Podcast clarity",
                         {{FilterType::HighPass, 80, 0.0, 0.71},
                          {FilterType::Peaking, 250, -2.5, 1.0},
                          {FilterType::Peaking, 3000, 3.0, 1.0},
                          {FilterType::Peaking, 6500, 0.0, 2.0, true, {DynMode::Above, -32, 4, -6, 1, 60}}}));
        // Dialogue forward; explosions reined in only while they happen.
        v.push_back(make(L"Movie dialogue",
                         {{FilterType::Peaking, 2500, 4.0, 0.8},
                          {FilterType::Peaking, 300, -1.5, 1.0},
                          {FilterType::LowShelf, 120, 0.0, 0.71, true, {DynMode::Above, -20, 3, -6, 15, 250}}}));
        // Less bass (more when it gets loud) for late listening; dialogue kept clear.
        v.push_back(make(L"Night mode",
                         {{FilterType::LowShelf, 110, -3.0, 0.71, true, {DynMode::Above, -24, 3, -6, 10, 200}},
                          {FilterType::Peaking, 2500, 3.0, 0.9}}));
        // Footsteps and reloads live around 2-5 kHz; rumble ducked when loud.
        v.push_back(make(L"Gaming: footsteps",
                         {{FilterType::LowShelf, 120, 0.0, 0.71, true, {DynMode::Above, -26, 4, -9, 5, 150}},
                          {FilterType::Peaking, 2000, 3.0, 1.0},
                          {FilterType::Peaking, 4500, 3.0, 1.2},
                          {FilterType::HighShelf, 12000, -2.0, 0.71}}));

        // ---- Speakers and listening comfort
        // Small drivers can't play deep bass (it only distorts): roll it off and
        // lift the upper bass that carries the impression of bass instead.
        v.push_back(make(L"Laptop / small speakers",
                         {{FilterType::HighPass, 100, 0.0, 0.71},
                          {FilterType::Peaking, 180, 3.0, 1.2},
                          {FilterType::Peaking, 450, -2.0, 1.4},
                          {FilterType::HighShelf, 9000, 2.0, 0.71}}));
        v.push_back(make(L"Warm (less fatigue)", {{FilterType::LowShelf, 180, 2.0, 0.71},
                                                  {FilterType::Peaking, 3000, -1.5, 0.9},
                                                  {FilterType::HighShelf, 8000, -3.0, 0.71}}));
        v.push_back(make(L"Bright / airy",
                         {{FilterType::HighShelf, 10000, 3.5, 0.71}, {FilterType::Peaking, 300, -1.0, 1.0}}));
        // Room boom controlled only when the bass gets loud.
        v.push_back(make(L"Boomy bass tamer (dynamic)",
                         {{FilterType::LowShelf, 120, 0.0, 0.71, true, {DynMode::Above, -22, 3, -6, 15, 200}}}));

        // ---- Repair
        // Single-band dynamic noise reduction: highs cut while they're quiet
        // (where hiss is audible), left alone when the music is bright.
        v.push_back(make(L"Hiss reducer (dynamic)",
                         {{FilterType::HighShelf, 6000, 0.0, 0.71, true, {DynMode::Below, -50, 2, -8, 5, 80}}}));
        // Mains hum and its next two harmonics (50 Hz grids: Europe, Asia, Africa, Australia).
        v.push_back(make(L"Hum removal (50 Hz)", {{FilterType::Notch, 50, 0.0, 8.0},
                                                  {FilterType::Notch, 100, 0.0, 8.0},
                                                  {FilterType::Notch, 150, 0.0, 8.0}}));
        // 60 Hz grids: the Americas, parts of Asia.
        v.push_back(make(L"Hum removal (60 Hz)", {{FilterType::Notch, 60, 0.0, 8.0},
                                                  {FilterType::Notch, 120, 0.0, 8.0},
                                                  {FilterType::Notch, 180, 0.0, 8.0}}));
        return v;
    }();
    return presets;
}

bool checkBuiltinPresets(std::wstring& report)
{
    int failures = 0;
    std::set<std::wstring> names;
    for (const BuiltinPreset& p : builtinPresets()) {
        std::wstring problems;
        auto fail = [&](const wchar_t* what) {
            problems += problems.empty() ? what : std::wstring(L"; ") + what;
        };
        const eqdsp::EqSettings& s = p.settings;
        if (!names.insert(p.name).second)
            fail(L"duplicate name");

        // Round trip through the preset text format.
        eqdsp::EqSettings back{};
        std::vector<std::string> warnings;
        const bool parsed = eqdsp::parseEqualizerApoConfig(eqdsp::formatEqualizerApoConfig(s), back, warnings);
        if (!parsed || !warnings.empty() || back.numBands != s.numBands)
            fail(L"doesn't round-trip through the preset format");
        for (uint32_t b = 0; b < s.numBands && b < back.numBands; ++b) {
            const eqdsp::Band& x = s.bands[b];
            const eqdsp::Band& y = back.bands[b];
            if (x.type != y.type || x.dynamic != y.dynamic || std::fabs(x.frequencyHz - y.frequencyHz) > 0.01 ||
                std::fabs(x.gainDb - y.gainDb) > 0.001 || (x.dynamic && (std::fabs(x.rangeDb - y.rangeDb) > 0.001 ||
                                                                         std::fabs(x.thresholdDb - y.thresholdDb) > 0.001)))
                fail(L"band changes in the round trip");
            if (x.frequencyHz < 10 || x.frequencyHz > 20000 || x.q < 0.1 || x.q > 24 || std::fabs(x.gainDb) > 24)
                fail(L"band parameter out of range");
        }

        // Can't clip: the preamp covers the largest boost (dynamic boosts included).
        const double boost = eqdsp::maxBoostDb(s, 2, 48000.0);
        if (s.preampDb > 0 || s.preampDb + boost > 0.11)
            fail(L"preamp doesn't cover the boost");

        // Processes loud noise without blowing up or clipping.
        eqdsp::Equalizer eq;
        eq.prepare(48000.0, 2);
        eq.setSettings(s, true);
        std::vector<float> buf(2 * 4800);
        uint32_t rng = 1;
        float peak = 0.0f;
        bool finite = true;
        for (int block = 0; block < 20; ++block) {
            for (float& x : buf) {
                rng = rng * 1664525u + 1013904223u;
                x = 0.25f * static_cast<float>(static_cast<int32_t>(rng)) / 2147483648.0f;
            }
            eq.process(buf.data(), buf.data(), 4800);
            for (float x : buf) {
                finite = finite && std::isfinite(x);
                peak = std::max(peak, std::fabs(x));
            }
        }
        if (!finite)
            fail(L"non-finite output");
        if (peak > 1.0f)
            fail(L"output clips");

        report += (problems.empty() ? L"  ok   " : L"  FAIL ") + p.name +
                  (problems.empty() ? std::wstring() : L": " + problems) + L"\n";
        failures += problems.empty() ? 0 : 1;
    }
    report += failures ? L"preset self-test FAILED\n" : L"preset self-test passed\n";
    return failures == 0;
}

} // namespace gui
