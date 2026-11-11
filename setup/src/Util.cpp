#include "Util.h"

#include "weq/Ids.h"

#include <algorithm>
#include <cwctype>

namespace weq {

std::wstring errorText(DWORD code)
{
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring s = n ? std::wstring(buf, n) : L"unknown error";
    if (buf)
        LocalFree(buf);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ' || s.back() == L'.'))
        s.pop_back();
    wchar_t code_[32];
    swprintf_s(code_, L" (%lu)", code);
    return s + code_;
}

std::wstring hrText(HRESULT hr)
{
    wchar_t buf[32];
    swprintf_s(buf, L"0x%08lX", static_cast<unsigned long>(hr));
    if (HRESULT_FACILITY(hr) == FACILITY_WIN32)
        return errorText(HRESULT_CODE(hr)) + L" [" + buf + L"]";
    return buf;
}

bool isElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

bool enablePrivilege(const wchar_t* name)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return false;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool ok = LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid) &&
              AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr) &&
              GetLastError() == ERROR_SUCCESS; // ERROR_NOT_ALL_ASSIGNED otherwise
    CloseHandle(token);
    return ok;
}

std::wstring exeDirectory()
{
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path, n);
    const size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : s.substr(0, slash);
}

std::wstring knownDir(const wchar_t* envVar, const wchar_t* fallback)
{
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(envVar, buf, MAX_PATH);
    return (n == 0 || n >= MAX_PATH) ? std::wstring(fallback) : std::wstring(buf, n);
}

std::wstring installDir()
{
    return knownDir(L"ProgramW6432", L"C:\\Program Files") + L"\\" + kInstallDirName;
}

std::wstring dataDir()
{
    return knownDir(L"ProgramData", L"C:\\ProgramData") + L"\\" + kInstallDirName;
}

std::wstring installedDllPath()
{
    return installDir() + L"\\" + kApoDllName;
}

std::wstring toLower(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

bool iequals(const std::wstring& a, const std::wstring& b)
{
    return toLower(a) == toLower(b);
}

bool icontains(const std::wstring& haystack, const std::wstring& needle)
{
    return toLower(haystack).find(toLower(needle)) != std::wstring::npos;
}

// ------------------------------------------------------------------ audio service

namespace {

// AudioEndpointBuilder owns and caches the endpoint (MMDevices) configuration,
// including FxProperties; Audiosrv depends on it. Restarting only Audiosrv
// leaves the cached effect settings in place, so restart the builder and
// everything that depends on it.
constexpr wchar_t kAudioService[] = L"AudioEndpointBuilder";

bool waitForState(SC_HANDLE svc, DWORD wanted, DWORD timeoutMs)
{
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    SERVICE_STATUS_PROCESS ssp{};
    DWORD needed = 0;
    while (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&ssp), sizeof(ssp), &needed)) {
        if (ssp.dwCurrentState == wanted)
            return true;
        if (GetTickCount64() > deadline)
            return false;
        Sleep(100);
    }
    return false;
}

bool stopOne(SC_HANDLE scm, const wchar_t* name, std::wstring& error)
{
    SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!svc) {
        error = std::wstring(L"open service ") + name + L": " + errorText(GetLastError());
        return false;
    }
    SERVICE_STATUS status{};
    bool ok = true;
    if (!ControlService(svc, SERVICE_CONTROL_STOP, &status)) {
        const DWORD err = GetLastError();
        if (err != ERROR_SERVICE_NOT_ACTIVE) {
            error = std::wstring(L"stop ") + name + L": " + errorText(err);
            ok = false;
        }
    }
    if (ok && !waitForState(svc, SERVICE_STOPPED, 20000)) {
        error = std::wstring(L"timed out waiting for ") + name + L" to stop";
        ok = false;
    }
    CloseServiceHandle(svc);
    return ok;
}

bool startOne(SC_HANDLE scm, const wchar_t* name, std::wstring& error)
{
    SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_START | SERVICE_QUERY_STATUS);
    if (!svc) {
        error = std::wstring(L"open service ") + name + L": " + errorText(GetLastError());
        return false;
    }
    bool ok = true;
    if (!StartServiceW(svc, 0, nullptr)) {
        const DWORD err = GetLastError();
        if (err != ERROR_SERVICE_ALREADY_RUNNING) {
            error = std::wstring(L"start ") + name + L": " + errorText(err);
            ok = false;
        }
    }
    if (ok && !waitForState(svc, SERVICE_RUNNING, 20000)) {
        error = std::wstring(L"timed out waiting for ") + name + L" to start";
        ok = false;
    }
    CloseServiceHandle(svc);
    return ok;
}

} // namespace

bool AudioServiceStopper::stop(std::wstring& error)
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        error = L"open service manager: " + errorText(GetLastError());
        return false;
    }

    // Remember running dependents so they can be restarted afterwards.
    dependents_.clear();
    if (SC_HANDLE svc = OpenServiceW(scm, kAudioService, SERVICE_ENUMERATE_DEPENDENTS)) {
        DWORD needed = 0, count = 0;
        EnumDependentServicesW(svc, SERVICE_ACTIVE, nullptr, 0, &needed, &count);
        if (GetLastError() == ERROR_MORE_DATA && needed) {
            std::vector<BYTE> buf(needed);
            auto* deps = reinterpret_cast<ENUM_SERVICE_STATUSW*>(buf.data());
            if (EnumDependentServicesW(svc, SERVICE_ACTIVE, deps, needed, &needed, &count))
                for (DWORD i = 0; i < count; ++i)
                    dependents_.emplace_back(deps[i].lpServiceName);
        }
        CloseServiceHandle(svc);
    }

    bool ok = true;
    for (const std::wstring& d : dependents_)
        ok = ok && stopOne(scm, d.c_str(), error);
    ok = ok && stopOne(scm, kAudioService, error);
    CloseServiceHandle(scm);

    stopped_ = true; // even on partial failure, try to restart on exit
    return ok;
}

bool AudioServiceStopper::restart(std::wstring& error)
{
    if (!stopped_)
        return true;
    stopped_ = false;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        error = L"open service manager: " + errorText(GetLastError());
        return false;
    }
    bool ok = startOne(scm, kAudioService, error);
    // EnumDependentServices lists dependents in reverse start order (stop
    // order), so start them back up from the end.
    for (auto it = dependents_.rbegin(); it != dependents_.rend(); ++it) {
        std::wstring depErr;
        if (!startOne(scm, it->c_str(), depErr) && ok) {
            error = depErr;
            ok = false;
        }
    }
    CloseServiceHandle(scm);
    return ok;
}

AudioServiceStopper::~AudioServiceStopper()
{
    std::wstring ignored;
    restart(ignored);
}

} // namespace weq
