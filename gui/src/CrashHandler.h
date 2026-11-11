#pragma once

#include <string>

namespace gui {

// Fatal errors: writes %APPDATA%\WindowsEQ\crashes\crash-<time>.dmp plus a
// line in crash.log (with the C++ exception message when there is one), then
// lets Windows Error Reporting run as usual. Call once at start-up.
void installCrashHandler();

// Non-fatal errors that were caught and survived: one line in
// %APPDATA%\WindowsEQ\error.log (kept under ~256 KB).
void logError(const wchar_t* where, const std::wstring& what);
void logError(const wchar_t* where, const char* what);

} // namespace gui
