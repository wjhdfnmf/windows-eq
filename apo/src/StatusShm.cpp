#include "StatusShm.h"

#include "ApoLog.h"
#include "weq/Ids.h"

#include <windows.h>
#include <sddl.h>

#include <cstring>

namespace weq {

namespace {

INIT_ONCE g_initOnce = INIT_ONCE_STATIC_INIT;
StatusBlock* g_block = nullptr; // mapped for the lifetime of the process

BOOL CALLBACK createMapping(PINIT_ONCE, PVOID, PVOID*)
{
    // SYSTEM, LOCAL SERVICE (audiodg) and Administrators: full access.
    // Authenticated users: read-only, so the control app can show status.
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;BA)(A;;GR;;;AU)", SDDL_REVISION_1, &sd, nullptr)) {
        apoLog(L"status: building security descriptor failed (%lu)", GetLastError());
        return TRUE;
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0,
                                        sizeof(StatusBlock), kStatusMappingName);
    const DWORD err = GetLastError();
    LocalFree(sd);

    if (!mapping) {
        apoLog(L"status: CreateFileMapping(%s) failed (%lu)", kStatusMappingName, err);
        return TRUE;
    }

    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(StatusBlock));
    if (!view) {
        apoLog(L"status: MapViewOfFile failed (%lu)", GetLastError());
        CloseHandle(mapping);
        return TRUE;
    }
    // Keep the mapping handle open (intentionally leaked) for the process lifetime.

    auto* block = static_cast<StatusBlock*>(view);
    if (err != ERROR_ALREADY_EXISTS || block->magic != kStatusMagic) {
        std::memset(block, 0, sizeof(StatusBlock));
        block->version = kStatusVersion;
        block->apoVersionMajor = kApoVersionMajor;
        block->apoVersionMinor = kApoVersionMinor;
        MemoryBarrier();
        block->magic = kStatusMagic;
    }
    g_block = block;
    apoLog(L"status: shared memory ready (%s)", err == ERROR_ALREADY_EXISTS ? L"opened existing" : L"created");
    return TRUE;
}

constexpr int32_t kClaiming = -1;

} // namespace

InstanceStatus* claimStatusSlot()
{
    InitOnceExecuteOnce(&g_initOnce, createMapping, nullptr, nullptr);
    if (!g_block)
        return nullptr;

    const DWORD pid = GetCurrentProcessId();
    for (InstanceStatus& s : g_block->instances) {
        const int32_t st = s.state;
        const bool free = st == kInstanceFree || (st != kClaiming && s.processId != pid); // stale: old audiodg
        if (!free)
            continue;
        if (InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&s.state), kClaiming, st) != st)
            continue;

        s.processId = pid;
        s.sampleRate = 0;
        s.channels = 0;
        s.processingMode = GUID{};
        s.framesProcessed = 0;
        s.lastProcessTick = 0;
        s.createdTick = GetTickCount64();
        s.discoveryOnly = 0;
        s.endpointId[0] = L'\0';
        MemoryBarrier();
        s.state = kInstanceInitialized;
        return &s;
    }
    apoLog(L"status: no free slot");
    return nullptr;
}

void releaseStatusSlot(InstanceStatus* slot)
{
    if (!slot)
        return;
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&slot->state), kInstanceFree);
}

} // namespace weq
