#pragma once

#include "eqdsp/Types.h"

#include <atomic>
#include <cstdint>
#include <cstring>

namespace weq {

// Hands EQ settings from the UI/control thread to the audio thread without
// locks (seqlock). The writer never blocks; the reader just tries again on its
// next period if it raced with a write.
class SettingsMailbox {
public:
    void publish(const eqdsp::EqSettings& s)
    {
        const uint32_t seq = seq_.load(std::memory_order_relaxed);
        seq_.store(seq + 1, std::memory_order_relaxed); // odd: writing
        std::atomic_thread_fence(std::memory_order_release);
        std::memcpy(&data_, &s, sizeof(s));
        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(seq + 2, std::memory_order_release); // even: stable
    }

    // Returns true (and fills `out`) only when there is a new, consistent value.
    bool tryTake(eqdsp::EqSettings& out, uint32_t& lastSeen) const
    {
        const uint32_t s1 = seq_.load(std::memory_order_acquire);
        if ((s1 & 1u) || s1 == lastSeen)
            return false;
        std::memcpy(&out, &data_, sizeof(out));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq_.load(std::memory_order_relaxed) != s1)
            return false;
        lastSeen = s1;
        return true;
    }

private:
    std::atomic<uint32_t> seq_{0};
    eqdsp::EqSettings data_{};
};

} // namespace weq
