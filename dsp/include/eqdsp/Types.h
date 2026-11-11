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
struct Band {
    FilterType type = FilterType::Peaking;
    uint32_t enabled = 1;
    uint32_t channelMask = kAllChannels;   // bit N = applies to channel N
    uint32_t reserved = 0;
    double frequencyHz = 1000.0;
    double gainDb = 0.0;                   // used by Peaking / shelves only
    double q = 0.70710678118654752;
};

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
