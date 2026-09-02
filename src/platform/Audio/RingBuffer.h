#pragma once
// =============================================================================
// Platform::Audio::RingBuffer — single-producer/single-consumer FIFO of
// interleaved audio samples, used between the audio thread (writer) and the
// SDL device callback / test sink (reader). Lock-free (atomics only).
//
// Not tied to SDL — tested in audio_test.cpp.
// =============================================================================

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Platform::Audio {

class RingBuffer {
public:
    explicit RingBuffer(size_t capacitySamples)
        : mCapacity(roundUpPow2(capacitySamples < 64 ? 64 : capacitySamples)),
          mBuffer(new int16_t[mCapacity]) {}

    ~RingBuffer() { delete[] mBuffer; }

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    // Writes interleaved samples. Returns the number of samples actually
    // written (0..count; the remainder is dropped on overflow).
    size_t write(const int16_t* samples, size_t count) {
        const size_t head = mHead.load(std::memory_order_relaxed);
        const size_t tail = mTail.load(std::memory_order_acquire);
        const size_t free = mCapacity - 1 - ((head - tail) & (mCapacity - 1));
        const size_t n = count < free ? count : free;
        for (size_t i = 0; i < n; ++i) {
            mBuffer[(head + i) & (mCapacity - 1)] = samples[i];
        }
        std::atomic_thread_fence(std::memory_order_release);
        mHead.store(head + n, std::memory_order_release);
        return n;
    }

    // Reads up to `count` samples into dst. Returns the number read.
    size_t read(int16_t* dst, size_t count) {
        const size_t tail = mTail.load(std::memory_order_relaxed);
        const size_t head = mHead.load(std::memory_order_acquire);
        const size_t avail = (head - tail) & (mCapacity - 1);
        const size_t n = count < avail ? count : avail;
        for (size_t i = 0; i < n; ++i) {
            dst[i] = mBuffer[(tail + i) & (mCapacity - 1)];
        }
        std::atomic_thread_fence(std::memory_order_release);
        mTail.store(tail + n, std::memory_order_release);
        return n;
    }

    size_t available() const {
        const size_t tail = mTail.load(std::memory_order_acquire);
        const size_t head = mHead.load(std::memory_order_acquire);
        return (head - tail) & (mCapacity - 1);
    }

    void clear() {
        const size_t head = mHead.load(std::memory_order_relaxed);
        mTail.store(head, std::memory_order_release);
    }

    size_t capacity() const { return mCapacity - 1; }

private:
    static size_t roundUpPow2(size_t v) {
        size_t p = 1;
        while (p < v) p <<= 1;
        return p;
    }

    size_t mCapacity;
    int16_t* mBuffer;
    alignas(64) std::atomic<size_t> mHead{0};
    alignas(64) std::atomic<size_t> mTail{0};
};

} // namespace Platform::Audio
