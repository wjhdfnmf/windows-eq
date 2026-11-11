#pragma once

#include "weq/Status.h"

namespace weq {

// Claims a free slot in the global status block (creating the section on
// first use). Returns nullptr if shared memory is unavailable or full.
// Not real-time safe; call from Initialize / constructor paths only.
InstanceStatus* claimStatusSlot();

void releaseStatusSlot(InstanceStatus* slot);

} // namespace weq
