#pragma once

// Identifiers shared by the APO DLL, the setup tool and the control app.

#include <guiddef.h>

namespace weq {

// {3193630D-A882-4724-8013-96C57C206F2A}
inline constexpr GUID kClsidApo = {0x3193630d, 0xa882, 0x4724, {0x80, 0x13, 0x96, 0xc5, 0x7c, 0x20, 0x6f, 0x2a}};
inline constexpr wchar_t kClsidApoString[] = L"{3193630D-A882-4724-8013-96C57C206F2A}";

inline constexpr wchar_t kApoFriendlyName[] = L"Windows EQ";
inline constexpr wchar_t kApoCopyright[] = L"Windows EQ project";
inline constexpr unsigned kApoVersionMajor = 0;
inline constexpr unsigned kApoVersionMinor = 5;

inline constexpr wchar_t kApoDllName[] = L"WindowsEqApo.dll";
inline constexpr wchar_t kInstallDirName[] = L"WindowsEQ";   // under %ProgramFiles% and %ProgramData%
inline constexpr wchar_t kLogFileName[] = L"apo.log";        // in %ProgramData%\WindowsEQ

// Our own bookkeeping (install state, per-device backups of original values).
inline constexpr wchar_t kProductRegKey[] = L"SOFTWARE\\WindowsEQ";

} // namespace weq
