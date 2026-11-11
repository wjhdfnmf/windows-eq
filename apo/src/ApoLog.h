#pragma once

#include <sal.h>

// Diagnostic log for the APO: %ProgramData%\WindowsEQ\apo.log (directory is
// created with write access for LOCAL SERVICE by the setup tool) plus
// OutputDebugString. NEVER call from the real-time thread (APOProcess).

namespace weq {

void apoLog(_Printf_format_string_ const wchar_t* fmt, ...);

} // namespace weq
