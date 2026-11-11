#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <vector>

namespace weq {

// Single-producer / single-consumer lock-free FIFO of interleaved float frames.
class FloatRing {
public:
    void init(size_t capacityFrames, int channels)
    {
        cap_ = capacityFrames;
        ch_ = static_cast<size_t>(channels);
        buf_.assign(cap_ * ch_, 0.0f);
        w_.store(0, std::memory_order_relaxed);
        r_.store(0, std::memory_order_relaxed);
    }

    size_t capacity() const { return cap_; }
    size_t readAvailable() const { return w_.load(std::memory_order_acquire) - r_.load(std::memory_order_acquire); }
    size_t writeAvailable() const { return cap_ - readAvailable(); }

    // src == nullptr writes silence.
    size_t write(const float* src, size_t frames)
    {
        const size_t w = w_.load(std::memory_order_relaxed);
        const size_t r = r_.load(std::memory_order_acquire);
        frames = std::min(frames, cap_ - (w - r));
        size_t done = 0;
        while (done < frames) {
            const size_t at = (w + done) % cap_;
            const size_t n = std::min(frames - done, cap_ - at);
            float* dst = &buf_[at * ch_];
            if (src)
                std::memcpy(dst, src + done * ch_, n * ch_ * sizeof(float));
            else
                std::fill(dst, dst + n * ch_, 0.0f);
            done += n;
        }
        w_.store(w + frames, std::memory_order_release);
        return frames;
    }

    // dst == nullptr discards.
    size_t read(float* dst, size_t frames)
    {
        const size_t r = r_.load(std::memory_order_relaxed);
        const size_t w = w_.load(std::memory_order_acquire);
        frames = std::min(frames, w - r);
        size_t done = 0;
        while (dst && done < frames) {
            const size_t at = (r + done) % cap_;
            const size_t n = std::min(frames - done, cap_ - at);
            std::memcpy(dst + done * ch_, &buf_[at * ch_], n * ch_ * sizeof(float));
            done += n;
        }
        r_.store(r + frames, std::memory_order_release);
        return frames;
    }

private:
    std::vector<float> buf_;
    size_t cap_ = 0, ch_ = 1;
    std::atomic<size_t> w_{0}, r_{0}; // monotonically increasing frame counters
};

} // namespace weq
