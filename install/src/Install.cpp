#include "weq/Install.h"

#include "weq/Version.h"

#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cwchar>
#include <iterator>

namespace weq::install {

namespace {

const wchar_t kUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\WindowsEQ";
const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t kRunValue[] = L"WindowsEQ";

std::wstring knownFolder(REFKNOWNFOLDERID id)
{
    wchar_t* p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p)) && p)
        s = p;
    CoTaskMemFree(p);
    return s;
}

bool setString(HKEY key, const wchar_t* name, const std::wstring& v)
{
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                          static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool setDword(HKEY key, const wchar_t* name, DWORD v)
{
    return RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v)) == ERROR_SUCCESS;
}

bool samePath(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

} // namespace

std::wstring installDir() { return knownFolder(FOLDERID_UserProgramFiles) + L"\\WindowsEQ"; }

std::wstring installedExePath() { return installDir() + L"\\" + kAppExe; }

std::wstring startMenuShortcutPath() { return knownFolder(FOLDERID_Programs) + L"\\Windows EQ.lnk"; }

std::wstring currentExePath()
{
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return std::wstring(buf, n);
}

bool isInstalled() { return GetFileAttributesW(installedExePath().c_str()) != INVALID_FILE_ATTRIBUTES; }

bool runningFromInstallDir() { return samePath(currentExePath(), installedExePath()); }

bool createShortcut(const std::wstring& target, const std::wstring& args, const std::wstring& linkPath,
                    const std::wstring& description)
{
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))))
        return false;
    link->SetPath(target.c_str());
    link->SetArguments(args.c_str());
    link->SetDescription(description.c_str());
    link->SetIconLocation(target.c_str(), 0);
    const size_t slash = target.find_last_of(L'\\');
    if (slash != std::wstring::npos)
        link->SetWorkingDirectory(target.substr(0, slash).c_str());
    IPersistFile* file = nullptr;
    bool ok = false;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)))) {
        ok = SUCCEEDED(file->Save(linkPath.c_str(), TRUE));
        file->Release();
    }
    link->Release();
    return ok;
}

bool registerUninstall(const std::wstring& exePath, unsigned long sizeKb)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return false;
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t date[16];
    swprintf(date, 16, L"%04u%02u%02u", st.wYear, st.wMonth, st.wDay);
    const std::wstring dir = exePath.substr(0, exePath.find_last_of(L'\\'));
    bool ok = setString(key, L"DisplayName", L"Windows EQ");
    ok &= setString(key, L"DisplayVersion", WEQ_VERSION_WSTR);
    ok &= setString(key, L"Publisher", L"" WEQ_PUBLISHER);
    ok &= setString(key, L"DisplayIcon", L"\"" + exePath + L"\",0");
    ok &= setString(key, L"InstallLocation", dir);
    ok &= setString(key, L"InstallDate", date);
    ok &= setString(key, L"UninstallString", L"\"" + exePath + L"\" --uninstall");
    ok &= setString(key, L"QuietUninstallString", L"\"" + exePath + L"\" --uninstall --quiet");
    ok &= setDword(key, L"NoModify", 1);
    ok &= setDword(key, L"NoRepair", 1);
    ok &= setDword(key, L"EstimatedSize", sizeKb);
    RegCloseKey(key);
    return ok;
}

void unregisterUninstall() { RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey); }

bool autostartEnabled()
{
    DWORD type = 0, size = 0;
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, &type, nullptr, &size) == ERROR_SUCCESS;
}

bool setAutostart(bool enable, const std::wstring& exePath)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return false;
    bool ok;
    if (enable)
        ok = setString(key, kRunValue, L"\"" + exePath + L"\" --tray");
    else {
        const LSTATUS s = RegDeleteValueW(key, kRunValue);
        ok = s == ERROR_SUCCESS || s == ERROR_FILE_NOT_FOUND;
    }
    RegCloseKey(key);
    return ok;
}

bool closeRunningInstance(DWORD timeoutMs)
{
    for (int attempt = 0; attempt < 4; ++attempt) {
        HWND w = FindWindowW(kMainWindowClass, nullptr);
        if (!w)
            return true;
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid == GetCurrentProcessId())
            return true;
        HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
        PostMessageW(w, kMsgQuit, 0, 0);
        if (proc) {
            const DWORD r = WaitForSingleObject(proc, timeoutMs);
            CloseHandle(proc);
            if (r != WAIT_OBJECT_0)
                return false;
        } else {
            Sleep(500);
        }
    }
    return FindWindowW(kMainWindowClass, nullptr) == nullptr;
}

void deleteDirectoryAfterExit(const std::wstring& dir)
{
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring cmd = L"\"" + std::wstring(sys) + L"\\cmd.exe\" /c ping 127.0.0.1 -n 3 > nul & rmdir /s /q \"" + dir +
                       L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, tmp, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

} // namespace weq::install
