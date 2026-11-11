#pragma once

// Per-user installation of the Windows EQ app: no admin rights needed.
//   %LOCALAPPDATA%\Programs\WindowsEQ\WindowsEQ.exe
//   Start menu shortcut, "Apps & features" entry (HKCU), optional sign-in launch (HKCU Run).

#include <windows.h>

#include <string>

namespace weq::install {

inline constexpr wchar_t kAppExe[] = L"WindowsEQ.exe";
inline constexpr wchar_t kMainWindowClass[] = L"WindowsEQ.Main";
// Posted to the main window: quit for real (closing only hides it to the tray).
inline constexpr UINT kMsgQuit = WM_APP + 20;
// Posted to the main window: show it (a second launch, or setup finished).
inline constexpr UINT kMsgShow = WM_APP + 21;

std::wstring installDir();
std::wstring installedExePath();
std::wstring startMenuShortcutPath();
std::wstring currentExePath();
bool isInstalled();
bool runningFromInstallDir();

bool createShortcut(const std::wstring& target, const std::wstring& args, const std::wstring& linkPath,
                    const std::wstring& description);

// "Apps & features" entry.
bool registerUninstall(const std::wstring& exePath, unsigned long sizeKb);
void unregisterUninstall();

// Launch at sign-in (HKCU\...\Run), starting hidden in the tray.
bool autostartEnabled();
bool setAutostart(bool enable, const std::wstring& exePath);

// Asks a running instance to quit and waits for its process to exit.
// Returns true if none is running afterwards.
bool closeRunningInstance(DWORD timeoutMs);

// Deletes a directory tree once this process has exited (it may contain the
// running exe): a hidden cmd.exe waits a moment and removes it.
void deleteDirectoryAfterExit(const std::wstring& dir);

} // namespace weq::install
