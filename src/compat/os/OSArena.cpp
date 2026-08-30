// =============================================================================
// compat/os — console RAM arena + boot header emulation.
//
// On the Wii, the OS carves the physical RAM into an "arena" that games use
// as raw heap memory (JKR's root heap is created from it). It also keeps the
// DOL boot header (OSBootInfo) at physical address 0, which JKRHeap reads to
// learn installed memory.
//
// On PC there is no physical RAM layout, so this file provides:
//   * a large lazily-committed virtual region ("console RAM"),
//   * the arena getters/setters the game calls (OSGetArenaLo/Hi, ...),
//   * OSInitAlloc (reserves the OS heap-descriptor table like RVL does),
//   * OSPhysicalToCached(0) -> a host-side OSBootInfo stub.
//
// The exact RVL behavior is mirrored where it is observable by the game; byte
// layout of the descriptor table is irrelevant because JKR never inspects it.
// =============================================================================

#include "compat/os/OSCompat.h"

#include "platform/platform.h"

#include <revolution.h>
#include <revolution/os/OSBootInfo.h>

#include <cstring>

namespace {
constexpr std::size_t kArenaTotalSize = 512ull * 1024 * 1024; // 512 MiB virtual
constexpr std::size_t kMem1Size = 128ull * 1024 * 1024;       // "main memory" arena
constexpr std::size_t kMem2Size = kArenaTotalSize - kMem1Size;

uint8_t* gArenaBase = nullptr; // start of reserved virtual region
uint8_t* sArenaLo = nullptr;   // MEM1 arena low (consumed by the game)
uint8_t* sArenaHi = nullptr;   // MEM1 arena high
uint8_t* sMem2Lo = nullptr;    // MEM2 (GDDR) arena low
uint8_t* sMem2Hi = nullptr;    // MEM2 arena high
OSBootInfo sBootInfo {};
} // namespace

extern "C" {

void* OSGetArenaLo() {
    return sArenaLo;
}

void* OSGetArenaHi() {
    return sArenaHi;
}

void OSSetArenaLo(void* lo) {
    sArenaLo = static_cast<uint8_t*>(lo);
}

void OSSetArenaHi(void* hi) {
    sArenaHi = static_cast<uint8_t*>(hi);
}

void* OSGetMEM2ArenaLo() {
    return sMem2Lo;
}

void* OSGetMEM2ArenaHi() {
    return sMem2Hi;
}

void OSSetMEM2ArenaLo(void* lo) {
    sMem2Lo = static_cast<uint8_t*>(lo);
}

void OSSetMEM2ArenaHi(void* hi) {
    sMem2Hi = static_cast<uint8_t*>(hi);
}

// Mirrors RVL OSInitAlloc: reserves a small descriptor table at the bottom of
// the arena (0x40 bytes per heap + 0x40, aligned to 0x20) and returns the new
// arena start. The game never touches the reserved table.
// PC_PORT: the alignment mask must be uintptr_t — `& ~0x1Fu` is a 32-bit mask
// that zero-extends and truncates 64-bit arena pointers (see types.h ALIGN_*).
void* OSInitAlloc(void* arenaStart, void* arenaEnd, s32 maxHeaps) {
    (void)arenaEnd;
    const u32 descSize = static_cast<u32>(maxHeaps) * 0x40u + 0x40u;
    const uintptr_t raw = reinterpret_cast<uintptr_t>(arenaStart);
    uint8_t* lo = reinterpret_cast<uint8_t*>((raw + descSize + 0x1Fu) & ~(uintptr_t)0x1Fu);
    OSSetArenaLo(lo);
    return lo;
}

// NOTE: OSPhysicalToCached is a MACRO in revolution/os.h (PPC address
// translation, `(u32)(paddr) + 0x80000000`) — it cannot be "implemented"
// here, and its semantics don't exist on a host. The game code's only use of
// it (JKRHeap::initArena reading the DOL boot header at physical 0) is
// patched in src/compat/patches/JSystem/JKernel/JKRHeap.cpp to call
// compat::getBootInfo() instead.

} // extern "C"

namespace compat {

void initOS() {
    if (gArenaBase != nullptr) {
        return; // already initialized
    }

    gArenaBase = static_cast<uint8_t*>(Platform::Memory::reserveArena(kArenaTotalSize, "console-ram"));
    if (!gArenaBase) {
        PL_LOG_FATAL("compat.os", "failed to reserve %zu MiB of virtual arena memory", kArenaTotalSize / (1024 * 1024));
        return;
    }

    // Split the region into "MEM1" (main) and "MEM2" (graphics) arenas. The
    // split is a logical partition only — both are plain host RAM.
    sArenaLo = gArenaBase;
    sArenaHi = gArenaBase + kMem1Size;
    sMem2Lo = gArenaBase + kMem1Size;
    sMem2Hi = gArenaBase + kArenaTotalSize;

    // DOL boot header stub (only `memorySize` is read by JKR).
    std::memset(&sBootInfo, 0, sizeof(sBootInfo));
    sBootInfo.memorySize = 0x04000000u; // 64 MiB — see README below
    sBootInfo.arenaLo = sArenaLo;
    sBootInfo.arenaHi = sArenaHi;

    PL_LOG_INFO("compat.os", "console RAM arena: %zu MiB (MEM1 %zu MiB, MEM2 %zu MiB)",
                kArenaTotalSize / (1024 * 1024), kMem1Size / (1024 * 1024), kMem2Size / (1024 * 1024));
}

std::size_t arenaSizeBytes() {
    return kArenaTotalSize;
}

const void* getBootInfo() {
    return &sBootInfo;
}

} // namespace compat
