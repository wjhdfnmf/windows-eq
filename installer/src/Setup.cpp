// WindowsEQ-Setup.exe: per-user installer with WindowsEQ.exe embedded as a
// resource. No administrator rights needed.
//
//   WindowsEQ-Setup.exe                    interactive
//   WindowsEQ-Setup.exe --silent [--autostart | --no-autostart]   (updates keep the current choice)
//   WindowsEQ-Setup.exe --extract-only DIR   (testing: just write the payload)

#include "AudioDevices.h"
#include "weq/Install.h"
#include "weq/Version.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <string>

namespace inst = weq::install;

namespace {

constexpr int kPayloadId = 100;
constexpr int kBtnInstall = 101;

bool hasArg(int argc, wchar_t** argv, const wchar_t* a)
{
    for (int i = 1; i < argc; ++i)
        if (_wcsicmp(argv[i], a) == 0)
            return true;
    return false;
}

bool cableInstalled()
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::wstring err;
    for (const auto& d : weq::listDevices(false, err))
        if (d.name.find(L"CABLE") != std::wstring::npos)
            return true;
    return false;
}

// Writes the embedded app to `path` (via a temp file, then an atomic replace).
bool extractPayload(const std::wstring& path, DWORD& sizeOut, std::wstring& error)
{
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(kPayloadId), RT_RCDATA);
    HGLOBAL data = res ? LoadResource(nullptr, res) : nullptr;
    const void* bytes = data ? LockResource(data) : nullptr;
    const DWORD size = res ? SizeofResource(nullptr, res) : 0;
    if (!bytes || size == 0) {
        error = L"the installer is damaged (no embedded app)";
        return false;
    }
    const std::wstring tmp = path + L".new";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = L"cannot write " + tmp;
        return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(h, bytes, size, &written, nullptr);
    CloseHandle(h);
    if (!ok || written != size) {
        DeleteFileW(tmp.c_str());
        error = L"cannot write " + tmp;
        return false;
    }
    // The old exe may still be closing; retry the replace for a few seconds.
    for (int i = 0; i < 30; ++i) {
        if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
            break;
        if (i == 29) {
            DeleteFileW(tmp.c_str());
            error = L"cannot replace " + path + L" (is Windows EQ still running?)";
            return false;
        }
        Sleep(200);
    }
    sizeOut = size;
    return true;
}

bool install(bool autostart, std::wstring& error)
{
    if (!inst::closeRunningInstance(8000)) {
        error = L"Windows EQ is running and didn't close. Quit it from its tray icon and try again.";
        return false;
    }
    const std::wstring dir = inst::installDir();
    const int mk = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    if (mk != ERROR_SUCCESS && mk != ERROR_ALREADY_EXISTS && mk != ERROR_FILE_EXISTS) {
        error = L"cannot create " + dir;
        return false;
    }
    const std::wstring exe = inst::installedExePath();
    DWORD size = 0;
    if (!extractPayload(exe, size, error))
        return false;
    if (!inst::createShortcut(exe, L"", inst::startMenuShortcutPath(), L"System-wide parametric equalizer")) {
        error = L"cannot create the Start menu shortcut";
        return false;
    }
    if (!inst::registerUninstall(exe, (size + 1023) / 1024)) {
        error = L"cannot register with Apps & features";
        return false;
    }
    if (!inst::setAutostart(autostart, exe)) {
        error = L"cannot change the sign-in setting";
        return false;
    }
    return true;
}

HRESULT CALLBACK dialogCallback(HWND, UINT msg, WPARAM, LPARAM lp, LONG_PTR)
{
    if (msg == TDN_HYPERLINK_CLICKED)
        ShellExecuteW(nullptr, L"open", reinterpret_cast<LPCWSTR>(lp), nullptr, nullptr, SW_SHOWNORMAL);
    return S_OK;
}

int interactive()
{
    const bool update = inst::isInstalled();
    const bool cable = cableInstalled();
    HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));

    std::wstring content =
        L"A system-wide parametric equalizer for everything you hear on this PC. It installs for your account "
        L"only (no administrator rights), adds a Start menu shortcut, and can be removed from Apps & features.\n\n";
    if (cable)
        content += L"✔ VB-Cable is installed.";
    else
        content += L"⚠ VB-Cable isn't installed. Windows EQ uses it to receive your audio: "
                   L"<a href=\"https://vb-audio.com/Cable/\">get VB-Cable</a> (free), install it, and choose "
                   L"\"CABLE Input\" as your Windows output. You can do that before or after this setup.";
    const std::wstring instruction = std::wstring(update ? L"Update Windows EQ to " : L"Install Windows EQ ") +
                                     WEQ_VERSION_WSTR;
    const std::wstring where = L"Installs to " + inst::installDir();

    TASKDIALOG_BUTTON buttons[] = {{kBtnInstall, update ? L"Update" : L"Install"}, {IDCANCEL, L"Cancel"}};
    TASKDIALOGCONFIG tdc{};
    tdc.cbSize = sizeof(tdc);
    tdc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_ENABLE_HYPERLINKS | TDF_USE_HICON_MAIN;
    const bool autoDefault = update ? inst::autostartEnabled() : true;
    if (autoDefault)
        tdc.dwFlags |= TDF_VERIFICATION_FLAG_CHECKED;
    tdc.pszWindowTitle = L"Windows EQ Setup";
    tdc.hMainIcon = icon;
    tdc.pszMainInstruction = instruction.c_str();
    tdc.pszContent = content.c_str();
    tdc.pszFooter = where.c_str();
    tdc.pButtons = buttons;
    tdc.cButtons = 2;
    tdc.nDefaultButton = kBtnInstall;
    tdc.pszVerificationText = L"Start Windows EQ when I sign in (it waits in the tray)";
    tdc.pfCallback = dialogCallback;

    int button = 0;
    BOOL autostart = FALSE;
    if (FAILED(TaskDialogIndirect(&tdc, &button, nullptr, &autostart)) || button != kBtnInstall)
        return 1;

    std::wstring error;
    if (!install(autostart != FALSE, error)) {
        const std::wstring msg = L"Setup couldn't finish: " + error;
        MessageBoxW(nullptr, msg.c_str(), L"Windows EQ Setup", MB_ICONERROR);
        return 2;
    }
    ShellExecuteW(nullptr, L"open", inst::installedExePath().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int rc = 0;
    if (argc >= 3 && _wcsicmp(argv[1], L"--extract-only") == 0) {
        DWORD size = 0;
        std::wstring error;
        SHCreateDirectoryExW(nullptr, argv[2], nullptr);
        rc = extractPayload(std::wstring(argv[2]) + L"\\" + inst::kAppExe, size, error) ? 0 : 2;
    } else if (hasArg(argc, argv, L"--silent")) {
        // Explicit flag wins; otherwise an update keeps the current choice and
        // a fresh install turns sign-in launch on.
        const bool autostart = hasArg(argc, argv, L"--autostart")      ? true
                               : hasArg(argc, argv, L"--no-autostart") ? false
                               : inst::isInstalled()                   ? inst::autostartEnabled()
                                                                       : true;
        std::wstring error;
        rc = install(autostart, error) ? 0 : 2;
    } else {
        rc = interactive();
    }
    LocalFree(argv);
    CoUninitialize();
    return rc;
}
