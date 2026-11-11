#pragma once

// Live status block the APO publishes in a global shared-memory section so
// the setup tool / control app can see which devices it is loaded on and
// whether audio is flowing. Written by audiodg.exe, read by anyone.

#include <cstdint>
#include <guiddef.h>

namespace weq {

inline constexpr wchar_t kStatusMappingName[] = L"Global\\WindowsEQ.Status.v1";
inline constexpr uint32_t kStatusMagic = 0x53514557; // 'WEQS'
inline constexpr uint32_t kStatusVersion = 1;
inline constexpr int kMaxStatusInstances = 32;

enum InstanceState : int32_t {
    kInstanceFree = 0,
    kInstanceInitialized = 1, // created and Initialize()d, not streaming
    kInstanceLocked = 2,      // LockForProcess() done: format fixed, processing
};

struct alignas(8) InstanceStatus {
    volatile int32_t state;          // InstanceState
    uint32_t processId;              // audiodg.exe PID that owns the slot
    uint32_t sampleRate;
    uint32_t channels;
    GUID processingMode;             // AUDIO_SIGNALPROCESSINGMODE_*
    volatile uint64_t framesProcessed;
    volatile uint64_t lastProcessTick; // GetTickCount64() at last APOProcess
    uint64_t createdTick;
    uint32_t discoveryOnly;          // instance created only to query capabilities
    uint32_t reserved;
    wchar_t endpointId[96];          // "{0.0.0.00000000}.{guid}"
};

struct StatusBlock {
    uint32_t magic;
    uint32_t version;
    uint32_t apoVersionMajor;
    uint32_t apoVersionMinor;
    InstanceStatus instances[kMaxStatusInstances];
};

} // namespace weq
