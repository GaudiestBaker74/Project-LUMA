// =============================================================================
// compat/os — OSMutex emulation.
//
// RVL's OSMutex is a count-based *recursive* mutex (see os_mutex.c: locking
// an already-owned mutex increments the count). std::recursive_mutex matches
// that semantics. The OSMutex struct itself (queue/thread/count) is unused —
// all access goes through the OS* functions — so we keep a registry keyed by
// address. Entries are never removed (game-lifetime objects; documented leak
// of a few dozen bytes per mutex).
//
// PC_PORT: registry entries are allocated with Platform::Memory::allocate
// (placement-new), NEVER with the global operator new. The global operator
// new routes through the JKR heap allocator, which itself calls OSLockMutex
// on the heap's mutex — allocating via operator new while holding
// gRegistryMutex would re-enter getMutex() on the same thread and deadlock.
// =============================================================================

#include <revolution.h>

#include "platform/Memory/Memory.h"

#include <cstring>
#include <mutex>
#include <new> // std::bad_alloc (MSVC does not pull <new> in transitively)
#include <unordered_map>

namespace {

// PC_PORT: allocator for the registry containers that routes through
// Platform::Memory instead of the global operator new. operator new goes
// through the JKR heap allocator, which itself locks heap mutexes via
// OSLockMutex → getMutex — allocating through operator new while holding
// gRegistryMutex would re-enter getMutex() on the same thread (deadlock on a
// non-recursive registry mutex, infinite recursion on a recursive one).
template <typename T>
struct PlatformAllocator {
    using value_type = T;

    PlatformAllocator() = default;
    template <typename U>
    PlatformAllocator(const PlatformAllocator<U>&) {}

    T* allocate(std::size_t n) {
        if (n == 0) {
            return nullptr;
        }
        void* p = Platform::Memory::allocate(n * sizeof(T), alignof(T));
        if (!p) {
            throw std::bad_alloc();
        }
        return static_cast<T*>(p);
    }

    void deallocate(T* p, std::size_t) {
        Platform::Memory::free(p);
    }

    template <typename U>
    bool operator==(const PlatformAllocator<U>&) const {
        return true;
    }
    template <typename U>
    bool operator!=(const PlatformAllocator<U>&) const {
        return false;
    }
};

std::mutex gRegistryMutex;
using MutexRegistry =
    std::unordered_map<OSMutex*, std::recursive_mutex*, std::hash<OSMutex*>,
                       std::equal_to<OSMutex*>, PlatformAllocator<std::pair<OSMutex* const, std::recursive_mutex*>>>;

// PC_PORT: the registry is a function-local (Meyers) singleton, NOT a
// namespace-scope global. Vendored global objects (JASHeapCtrl.cpp's
// `JASHeap JASKernel::audioAramHeap`) call OSInitMutex during static
// initialization, before file-scope statics of other TUs are guaranteed to
// be constructed — a global unordered_map is still empty-but-unconstructed
// at that point (bucket count 0) and the first emplace divides by zero
// (SIGFPE before main). The function-local static is constructed on first
// use, which is always safe (thread-safe statics guard).
MutexRegistry& mutexRegistry() {
    static MutexRegistry sRegistry;
    return sRegistry;
}

std::recursive_mutex* getMutex(OSMutex* mutex) {
    std::lock_guard<std::mutex> lock(gRegistryMutex);
    MutexRegistry& registry = mutexRegistry();
    auto it = registry.find(mutex);
    if (it != registry.end()) {
        return it->second;
    }
    void* storage = Platform::Memory::allocate(sizeof(std::recursive_mutex),
                                               alignof(std::recursive_mutex));
    auto* created = ::new (storage) std::recursive_mutex();
    registry.emplace(mutex, created);
    return created;
}
} // namespace

extern "C" {

void OSInitMutex(OSMutex* mutex) {
    if (!mutex) {
        return;
    }
    // Keep the struct deterministic even though we don't use its fields.
    std::memset(mutex, 0, sizeof(*mutex));
    getMutex(mutex);
}

void OSLockMutex(OSMutex* mutex) {
    if (!mutex) {
        return;
    }
    getMutex(mutex)->lock();
}

void OSUnlockMutex(OSMutex* mutex) {
    if (!mutex) {
        return;
    }
    getMutex(mutex)->unlock();
}

BOOL OSTryLockMutex(OSMutex* mutex) {
    if (!mutex) {
        return FALSE;
    }
    return getMutex(mutex)->try_lock() ? TRUE : FALSE;
}

} // extern "C"
