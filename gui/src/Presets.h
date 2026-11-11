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

} // namespace gui
