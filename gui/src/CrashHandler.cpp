#include "CrashHandler.h"

#include "Storage.h"
#include "weq/Version.h"

#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <cwchar>
#include <exception>

namespace gui {

namespace {

wchar_t g_dir[MAX_PATH] = L"";     // ...\WindowsEQ (resolved at start-up: no allocation when crashing)
wchar_t g_crashDir[MAX_PATH] = L""; // ...\WindowsEQ\crashes
LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;
volatile LONG g_dumped = 0;

void timestamp(wchar_t* out, size_t n, bool forFileName)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    swprintf(out, n, forFileName ? L"%04u%02u%02u-%02u%02u%02u" : L"%04u-%02u-%02u %02u:%02u:%02u", st.wYear,
             st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

void appendLine(const wchar_t* file, const wchar_t* line)
{
    HANDLE h = CreateFileW(file, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    char utf8[2048];
    const int len = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8) - 2, nullptr, nullptr);
    if (len > 1) {
        utf8[len - 1] = '\r';
        utf8[len] = '\n';
        DWORD w = 0;
        WriteFile(h, utf8, static_cast<DWORD>(len + 1), &w, nullptr);
    }
    CloseHandle(h);
}

void writeDump(EXCEPTION_POINTERS* ep)
{
    if (InterlockedExchange(&g_dumped, 1) != 0 || !g_crashDir[0])
        return;
    CreateDirectoryW(g_crashDir, nullptr);
    wchar_t ts[32], path[MAX_PATH];
    timestamp(ts, 32, true);
    swprintf(path, MAX_PATH, L"%s\\crash-%s.dmp", g_crashDir, ts);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return;
    MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                      static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
                      ep ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(f);
}

void logCrash(const wchar_t* what)
{
    if (!g_crashDir[0])
        return;
    CreateDirectoryW(g_crashDir, nullptr);
    wchar_t ts[32], line[1024], file[MAX_PATH];
    timestamp(ts, 32, false);
    swprintf(line, 1024, L"%s  Windows EQ %s  %s", ts, WEQ_VERSION_WSTR, what);
    swprintf(file, MAX_PATH, L"%s\\crash.log", g_crashDir);
    appendLine(file, line);
}

LONG WINAPI unhandledFilter(EXCEPTION_POINTERS* ep)
{
    wchar_t what[160];
    const auto* rec = ep ? ep->ExceptionRecord : nullptr;
    HMODULE mod = nullptr;
    if (rec)
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(rec->ExceptionAddress), &mod);
    swprintf(what, 160, L"unhandled exception 0x%08lX at module+0x%llX", rec ? rec->ExceptionCode : 0ul,
             rec ? static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(rec->ExceptionAddress) -
                                                   reinterpret_cast<uintptr_t>(mod))
                 : 0ull);
    logCrash(what);
    writeDump(ep);
    // Chain to the C++ runtime's filter (it turns C++ exceptions into
    // std::terminate, which logs the message below) and then to WER.
    return g_previous ? g_previous(ep) : EXCEPTION_CONTINUE_SEARCH;
}

[[noreturn]] void onTerminate()
{
    wchar_t what[600] = L"std::terminate";
    if (std::exception_ptr ex = std::current_exception()) {
        try {
            std::rethrow_exception(ex);
        } catch (const std::exception& e) {
            swprintf(what, 600, L"uncaught C++ exception: %hs", e.what());
        } catch (...) {
            swprintf(what, 600, L"uncaught C++ exception of unknown type");
        }
    }
    logCrash(what);
    writeDump(nullptr);
    std::abort();
}

} // namespace

void installCrashHandler()
{
    const std::wstring dir = appDataDir();
    wcsncpy_s(g_dir, dir.c_str(), _TRUNCATE);
    swprintf(g_crashDir, MAX_PATH, L"%s\\crashes", g_dir);
    g_previous = SetUnhandledExceptionFilter(unhandledFilter);
    std::set_terminate(onTerminate);
}

void logError(const wchar_t* where, const std::wstring& what)
{
    if (!g_dir[0])
        return;
    wchar_t file[MAX_PATH];
    swprintf(file, MAX_PATH, L"%s\\error.log", g_dir);
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (GetFileAttributesExW(file, GetFileExInfoStandard, &a) && a.nFileSizeLow > 256 * 1024)
        DeleteFileW(file);
    wchar_t ts[32], line[1024];
    timestamp(ts, 32, false);
    swprintf(line, 1024, L"%s  Windows EQ %s  %s: %s", ts, WEQ_VERSION_WSTR, where, what.c_str());
    appendLine(file, line);
}

void logError(const wchar_t* where, const char* what)
{
    wchar_t w[600];
    swprintf(w, 600, L"%hs", what ? what : "?");
    logError(where, std::wstring(w));
}

} // namespace gui
