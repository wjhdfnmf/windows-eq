#include "ApoLog.h"

#include "weq/Ids.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cwchar>

namespace weq {

namespace {

SRWLOCK g_logLock = SRWLOCK_INIT;
constexpr LONGLONG kMaxLogBytes = 1024 * 1024;

bool logPath(wchar_t* out, size_t cch, const wchar_t* fileName)
{
    wchar_t programData[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"ProgramData", programData, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        wcscpy_s(programData, L"C:\\ProgramData");
    return swprintf_s(out, cch, L"%s\\%s\\%s", programData, kInstallDirName, fileName) > 0;
}

} // namespace

void apoLog(const wchar_t* fmt, ...)
{
    wchar_t msg[1024];
    va_list args;
    va_start(args, fmt);
    vswprintf_s(msg, fmt, args);
    va_end(args);

    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t line[1200];
    const int len = swprintf_s(line, L"%04u-%02u-%02u %02u:%02u:%02u.%03u [pid %lu tid %lu] %s\r\n",
                               t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                               t.wMilliseconds, GetCurrentProcessId(), GetCurrentThreadId(), msg);
    if (len <= 0)
        return;

    OutputDebugStringW(line);

    char utf8[2400];
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, line, len, utf8, sizeof(utf8), nullptr, nullptr);
    if (bytes <= 0)
        return;

    wchar_t path[MAX_PATH], oldPath[MAX_PATH];
    if (!logPath(path, MAX_PATH, kLogFileName))
        return;

    AcquireSRWLockExclusive(&g_logLock);

    // Simple rotation: apo.log -> apo.log.old once it passes 1 MB.
    WIN32_FILE_ATTRIBUTE_DATA attr;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &attr)) {
        const LONGLONG size = (static_cast<LONGLONG>(attr.nFileSizeHigh) << 32) | attr.nFileSizeLow;
        if (size > kMaxLogBytes && swprintf_s(oldPath, L"%s.old", path) > 0)
            MoveFileExW(path, oldPath, MOVEFILE_REPLACE_EXISTING);
    }

    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, utf8, static_cast<DWORD>(bytes), &written, nullptr);
        CloseHandle(h);
    }

    ReleaseSRWLockExclusive(&g_logLock);
}

} // namespace weq
