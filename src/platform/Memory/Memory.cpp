#include "platform/Memory/Memory.h"

#include "platform/PlatformDetail.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>

namespace Platform::Memory {
namespace {

// Header stored just before the aligned user pointer.
struct BlockHeader {
    void* raw;         // original malloc() pointer
    size_t size;       // requested size
    size_t alignment;  // requested alignment
};

std::atomic<Stats> gStats;

void trackAllocate(size_t size) {
    Stats expected = gStats.load(std::memory_order_relaxed);
    Stats desired;
    do {
        desired = expected;
        desired.allocationCount += 1;
        desired.totalBytesAllocated += size;
        desired.bytesAllocated += size;
        if (desired.bytesAllocated > desired.bytesPeak) {
            desired.bytesPeak = desired.bytesAllocated;
        }
    } while (!gStats.compare_exchange_weak(expected, desired, std::memory_order_relaxed));
}

void trackFree(size_t size) {
    Stats expected = gStats.load(std::memory_order_relaxed);
    Stats desired;
    do {
        desired = expected;
        desired.allocationCount -= 1;  // outstanding allocations
        desired.deallocationCount += 1;
        desired.bytesAllocated -= size;
    } while (!gStats.compare_exchange_weak(expected, desired, std::memory_order_relaxed));
}

bool isValidAlignment(size_t alignment) {
    return alignment != 0 && (alignment & (alignment - 1)) == 0;
}

} // namespace

void* allocate(size_t size, size_t alignment) {
    if (size == 0) {
        return nullptr;
    }
    if (!isValidAlignment(alignment)) {
        return nullptr;
    }
    if (alignment < alignof(BlockHeader)) {
        alignment = alignof(BlockHeader);
    }

    // Total block: header + worst-case padding + payload. `raw` is the
    // malloc() pointer that free() must reclaim.
    const size_t headerSize = sizeof(BlockHeader);
    void* raw = std::malloc(headerSize + (alignment - 1) + size);
    if (!raw) {
        return nullptr;
    }

    uintptr_t aligned =
        (reinterpret_cast<uintptr_t>(raw) + headerSize + alignment - 1) & ~(alignment - 1);
    auto* header = reinterpret_cast<BlockHeader*>(aligned - sizeof(BlockHeader));
    header->raw = raw;
    header->size = size;
    header->alignment = alignment;

    std::memset(reinterpret_cast<void*>(aligned), 0, size);
    trackAllocate(size);
    return reinterpret_cast<void*>(aligned);
}

void* callocate(size_t size, size_t alignment) {
    return allocate(size, alignment); // allocate() already zeroes
}

void free(void* ptr) {
    if (!ptr) {
        return;
    }
    auto* header = reinterpret_cast<BlockHeader*>(reinterpret_cast<uintptr_t>(ptr) - sizeof(BlockHeader));
    const size_t size = header->size;
    std::free(header->raw);
    trackFree(size);
}

void* reallocate(void* ptr, size_t newSize) {
    if (!ptr) {
        return allocate(newSize);
    }
    if (newSize == 0) {
        free(ptr);
        return nullptr;
    }
    const size_t oldSize = sizeOf(ptr);
    void* fresh = allocate(newSize);
    if (!fresh) {
        return nullptr;
    }
    std::memcpy(fresh, ptr, oldSize < newSize ? oldSize : newSize);
    free(ptr);
    return fresh;
}

size_t sizeOf(const void* ptr) {
    if (!ptr) {
        return 0;
    }
    auto* header = reinterpret_cast<const BlockHeader*>(reinterpret_cast<uintptr_t>(ptr) - sizeof(BlockHeader));
    return header->size;
}

Stats stats() {
    return gStats.load(std::memory_order_relaxed);
}

void* reserveArena(size_t size, const char* purpose) {
    void* base = Platform::Detail::reserveVirtual(size, purpose);
    if (!base) {
        return nullptr;
    }
    Stats expected = gStats.load(std::memory_order_relaxed);
    Stats desired;
    do {
        desired = expected;
        desired.arenaBytes += size;
    } while (!gStats.compare_exchange_weak(expected, desired, std::memory_order_relaxed));
    return base;
}

void releaseArena(void* base, size_t size) {
    if (!base) {
        return;
    }
    Platform::Detail::releaseVirtual(base, size);
    Stats expected = gStats.load(std::memory_order_relaxed);
    Stats desired;
    do {
        desired = expected;
        desired.arenaBytes = desired.arenaBytes > size ? desired.arenaBytes - size : 0;
    } while (!gStats.compare_exchange_weak(expected, desired, std::memory_order_relaxed));
}

} // namespace Platform::Memory
