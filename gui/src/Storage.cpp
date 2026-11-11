#include "Storage.h"

#include <commdlg.h>
#include <shlobj.h>

#include <algorithm>
#include <cwctype>

namespace gui {

std::wstring appDataDir()
{
    wchar_t* path = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &path)) && path)
        dir = std::wstring(path) + L"\\WindowsEQ";
    CoTaskMemFree(path);
    if (!dir.empty())
        CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring currentPresetPath() { return appDataDir() + L"\\eq.txt"; }

std::wstring presetsDir()
{
    const std::wstring d = appDataDir() + L"\\presets";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

std::wstring configPath() { return appDataDir() + L"\\config.ini"; }

bool readTextFile(const std::wstring& path, std::string& out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    out.resize(static_cast<size_t>(std::min<LONGLONG>(size.QuadPart, 1 << 20)));
    DWORD read = 0;
    const BOOL ok = out.empty() || ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
    CloseHandle(h);
    out.resize(read);
    return ok != FALSE;
}

bool writeTextFileAtomic(const std::wstring& path, const std::string& data)
{
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    CloseHandle(h);
    if (!ok || written != data.size()) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

unsigned long long fileStamp(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a))
        return 0;
    return (static_cast<unsigned long long>(a.ftLastWriteTime.dwHighDateTime) << 32 |
            a.ftLastWriteTime.dwLowDateTime) ^
           a.nFileSizeLow;
}

std::vector<std::wstring> listUserPresets()
{
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((presetsDir() + L"\\*.txt").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                names.push_back(fileStem(fd.cFileName));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });
    return names;
}

std::wstring sanitizeFileName(const std::wstring& name)
{
    std::wstring out;
    for (wchar_t c : name)
        out += (wcschr(L"<>:\"/\\|?*", c) || c < 32) ? L'_' : c;
    while (!out.empty() && (out.back() == L' ' || out.back() == L'.'))
        out.pop_back();
    while (!out.empty() && out.front() == L' ')
        out.erase(out.begin());
    return out.substr(0, 80);
}

std::wstring userPresetPath(const std::wstring& name) { return presetsDir() + L"\\" + sanitizeFileName(name) + L".txt"; }

std::wstring configGet(const wchar_t* key, const std::wstring& def)
{
    wchar_t buf[1024];
    GetPrivateProfileStringW(L"WindowsEQ", key, def.c_str(), buf, 1024, configPath().c_str());
    return buf;
}

void configSet(const wchar_t* key, const std::wstring& value)
{
    WritePrivateProfileStringW(L"WindowsEQ", key, value.c_str(), configPath().c_str());
}

static const wchar_t kFilter[] = L"EQ presets (*.txt)\0*.txt\0All files (*.*)\0*.*\0";

std::wstring openPresetDialog(HWND owner)
{
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = kFilter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Import EQ preset (Equalizer APO / AutoEQ ParametricEQ.txt)";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&ofn) ? std::wstring(file) : std::wstring();
}

std::wstring savePresetDialog(HWND owner, const std::wstring& suggestedName)
{
    wchar_t file[MAX_PATH] = L"";
    wcsncpy_s(file, sanitizeFileName(suggestedName.empty() ? L"My EQ" : suggestedName).c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = kFilter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"txt";
    ofn.lpstrTitle = L"Export EQ preset";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    return GetSaveFileNameW(&ofn) ? std::wstring(file) : std::wstring();
}

std::wstring fileStem(const std::wstring& path)
{
    size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0)
        name.resize(dot);
    return name;
}

} // namespace gui
