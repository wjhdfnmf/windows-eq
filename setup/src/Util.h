#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace weq {

std::wstring errorText(DWORD code);           // Win32 error -> message
std::wstring hrText(HRESULT hr);

bool isElevated();
bool enablePrivilege(const wchar_t* name);    // SE_BACKUP_NAME, SE_RESTORE_NAME, ...

std::wstring exeDirectory();
std::wstring knownDir(const wchar_t* envVar, const wchar_t* fallback); // %ProgramFiles% etc.
std::wstring installDir();                    // %ProgramFiles%\WindowsEQ
std::wstring dataDir();                       // %ProgramData%\WindowsEQ
std::wstring installedDllPath();

std::wstring toLower(std::wstring s);
bool iequals(const std::wstring& a, const std::wstring& b);
bool icontains(const std::wstring& haystack, const std::wstring& needle);

// Stops the Windows Audio service (and anything depending on it), which also
// terminates audiodg.exe so the APO DLL is unloaded. Restarts on destruction
// unless restart() was already called.
class AudioServiceStopper {
public:
    AudioServiceStopper() = default;
    ~AudioServiceStopper();
    AudioServiceStopper(const AudioServiceStopper&) = delete;
    AudioServiceStopper& operator=(const AudioServiceStopper&) = delete;

    bool stop(std::wstring& error);
    bool restart(std::wstring& error);

private:
    bool stopped_ = false;
    std::vector<std::wstring> dependents_;
};

} // namespace weq
