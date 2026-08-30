#pragma once
// =============================================================================
// Platform::Memory — raw memory management + statistics.
//
// This is the backing layer for the game's allocators (the Wii compatibility
// layer and, indirectly, the JKR heap system of the game get their arena from
// here). It deliberately does NOT replace JKR heaps (see docs/architecture.md
// ADR-004): the game keeps using its own heaps, which are backed by memory
// obtained through this module.
// =============================================================================

#include <cstddef>
#include <cstdint>

namespace Platform::Memory {

struct Stats {
    uint64_t allocationCount = 0;    // outstanding allocations
    uint64_t deallocationCount = 0;  // total frees
    uint64_t bytesAllocated = 0;     // outstanding bytes
    uint64_t bytesPeak = 0;          // high-water mark of bytesAllocated
    uint64_t totalBytesAllocated = 0; // lifetime sum of allocation sizes
    uint64_t arenaBytes = 0;         // reserved arena memory (excluded from the above)
};

// Allocates `size` bytes, aligned to at least `alignment` (power of two,
// default 16). Returns nullptr for size == 0. Allocations are zero-initialized
// for safety (the game often assumes fresh memory).
void* allocate(size_t size, size_t alignment = 16);

// Frees a pointer from allocate()/callocate()/reallocate(). nullptr is a no-op.
void free(void* ptr);

// Allocates and zeroes. Equivalent to allocate(size) (which already zeroes).
void* callocate(size_t size, size_t alignment = 16);

// Grows/shrinks a previous allocation (new block + copy; old block freed).
void* reallocate(void* ptr, size_t newSize);

// Size of the allocation that `ptr` points to (0 if unknown).
size_t sizeOf(const void* ptr);

// Global statistics (atomic, thread-safe).
Stats stats();

// --- Arena memory -----------------------------------------------------------
// Reserved, lazily-committed virtual address space. Used by compat/os to
// emulate the console's RAM arena. Implemented per platform:
//   linux:   mmap(PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS)
//   windows: VirtualAlloc(MEM_RESERVE|MEM_COMMIT)
// Not counted in the allocation stats (only in Stats::arenaBytes).
void* reserveArena(size_t size, const char* purpose);
void releaseArena(void* base, size_t size);

// --- Alignment helpers ------------------------------------------------------
constexpr size_t alignUp(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

constexpr bool isAligned(size_t value, size_t alignment) {
    return (value & (alignment - 1)) == 0;
}

} // namespace Platform::Memory
