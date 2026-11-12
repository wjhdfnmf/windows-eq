#pragma once

#include "eqdsp/Types.h"

#include <string>
#include <vector>

namespace gui {

struct BuiltinPreset {
    std::wstring name;
    eqdsp::EqSettings settings;
};

// Starting points that are easy to hear; each has its preamp set so it
// cannot clip.
const std::vector<BuiltinPreset>& builtinPresets();

// Self-test: every built-in preset round-trips through the preset text format,
// has sane parameters, can't clip (preamp covers the largest boost, dynamic
// boosts included) and processes audio cleanly. Appends to `report`.
bool checkBuiltinPresets(std::wstring& report);

} // namespace gui
