#pragma once

#include "eqdsp/Types.h"

#include <string>
#include <string_view>
#include <vector>

namespace eqdsp {

// Parses the Equalizer APO configuration text format, which is also what
// AutoEQ exports as "ParametricEQ.txt". Supported subset:
//
//   Preamp: -6.2 dB
//   Filter 1: ON PK Fc 1000 Hz Gain -3.5 dB Q 1.41
//   Filter: ON LSC Fc 105 Hz Gain 6.0 dB Q 0.70      (LS/LSC, HS/HSC)
//   Filter: ON LP Fc 18000 Hz                       (LP/HP default Q 0.7071; LPQ/HPQ take Q)
//   Filter: ON BP|NO|AP Fc 3000 Hz Q 2
//   Filter: ON PK Fc 1000 Hz Gain 3 dB BW Oct 1.0   (bandwidth in octaves instead of Q)
//   Channel: L R | all | 1 2 ...                    (applies to following filters)
//   # comment
//
// Filters marked OFF are kept as disabled bands. Unknown or malformed lines
// are skipped and reported in `warnings` (with line numbers). Returns false
// only if nothing usable was found in non-empty input.
bool parseEqualizerApoConfig(std::string_view text, EqSettings& out, std::vector<std::string>& warnings);

// Serialises settings back to the same format (round-trips through the parser).
std::string formatEqualizerApoConfig(const EqSettings& s);

} // namespace eqdsp
