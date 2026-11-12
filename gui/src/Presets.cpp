#include "Presets.h"

#include "eqdsp/BiquadDesign.h"

#include <cmath>
#include <initializer_list>

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
        return v;
    }();
    return presets;
}

} // namespace gui
