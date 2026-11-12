#pragma once

#include <cstdint>
#include <type_traits>

namespace eqdsp {

constexpr int kMaxBands = 32;
constexpr int kMaxChannels = 32;           // one bit per channel in Band::channelMask
constexpr uint32_t kAllChannels = 0xFFFFFFFFu;

enum class FilterType : uint32_t {
    Peaking = 0,
    LowShelf,
    HighShelf,
    LowPass,
    HighPass,
    BandPass,   // constant 0 dB peak gain
    Notch,
    AllPass,
};

// Plain, fixed-layout structs so they can be placed in shared memory between
// the control app and the APO later. Booleans are uint32_t for stable layout.
enum class DynMode : uint32_t {
    Above = 0, // act while the band's level is above the threshold (tame, de-ess)
    Below = 1, // act while it is below the threshold (lift quiet content)
};

struct Band {
    FilterType type = FilterType::Peaking;
    uint32_t enabled = 1;
    uint32_t channelMask = kAllChannels;   // bit N = applies to channel N
    uint32_t dynamic = 0;                  // 1 = dynamic band (Peaking / shelves only)
    double frequencyHz = 1000.0;
    double gainDb = 0.0;                   // used by Peaking / shelves only
    double q = 0.70710678118654752;

    // Dynamics: the band's level (measured through a band-pass at the band
    // for peaks, or a low/high-pass for shelves) moves its gain between
    // gainDb and gainDb + rangeDb.
    double thresholdDb = -30.0;            // dBFS (sine-referenced), 6 dB soft knee
    double ratio = 2.0;                    // >= 1; gain change = excess * (1 - 1/ratio)
    double rangeDb = -6.0;                 // max gain change; < 0 cuts, > 0 boosts
    double attackMs = 5.0;
    double releaseMs = 120.0;
    DynMode dynMode = DynMode::Above;
    uint32_t dynReserved = 0;
};

inline bool supportsDynamics(FilterType t)
{
    return t == FilterType::Peaking || t == FilterType::LowShelf || t == FilterType::HighShelf;
}
inline bool isDynamic(const Band& b) { return b.dynamic != 0 && b.enabled != 0 && supportsDynamics(b.type); }

struct EqSettings {
    uint32_t numBands = 0;
    uint32_t bypass = 0;
    uint32_t limiterEnabled = 1;
    uint32_t reserved = 0;
    double preampDb = 0.0;
    double limiterCeilingDb = 0.0;         // dBFS; limiter only acts above this
    Band bands[kMaxBands];
};

static_assert(std::is_trivially_copyable_v<Band>);
static_assert(std::is_trivially_copyable_v<EqSettings>);

} // namespace eqdsp
