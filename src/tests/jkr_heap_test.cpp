// =============================================================================
// Smoke test: real Petari (vendored) code compiled natively.
//
// This test compiles three unmodified JSystem heap sources from
// third_party/petari (JKRHeap.cpp, JKRExpHeap.cpp, JKRDisposer.cpp) together
// with the compat/os layer, and exercises the game's own memory allocator the
// same way the game does at boot:
//
//   JKRExpHeap::createRoot()  ->  JKRHeap::initArena() (compat arena)
//   JKRExpHeap::create()      ->  child heap
//   heap->alloc/free/freeAll  ->  the game's block allocator
//
// Requires compat::initOS() to have run first (done in the test binary main).
// =============================================================================

#include "tests/test_runner.h"

#include "platform/Memory/Memory.h"

#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>

#include <cstdint>
#include <cstring>

TEST_CASE(jkr_expheap_smoke) {
    // The root heap must not exist yet (fresh process, arena set up by main).
    CHECK(JKRHeap::sRootHeap == nullptr);

    // 1. Root heap from the compat arena, exactly like the game boot does.
    JKRExpHeap* root = JKRExpHeap::createRoot(1, true);
    REQUIRE(root != nullptr);
    CHECK(JKRHeap::sRootHeap == root);
    CHECK(JKRHeap::sSystemHeap == root);

    // 2. Child heap of 1 MiB carved out of the root.
    JKRExpHeap* child = JKRExpHeap::create(0x100000, root, true);
    REQUIRE(child != nullptr);
    CHECK(child->getParent() == root);

    // 3. Basic allocation.
    void* block = child->alloc(256, 4);
    REQUIRE(block != nullptr);
    CHECK(Platform::Memory::isAligned(reinterpret_cast<uintptr_t>(block), 4));
    std::memset(block, 0xCD, 256);

    // 4. Aligned allocation (4 KiB aligned, like texture data would be).
    void* aligned = child->alloc(0x1000, 0x100);
    REQUIRE(aligned != nullptr);
    CHECK((reinterpret_cast<uintptr_t>(aligned) & 0xFF) == 0);
    std::memset(aligned, 0xAB, 0x1000);

    // 5. The heap must report less free space after allocations.
    const s32 freeAfterAlloc = child->getFreeSize();
    CHECK(freeAfterAlloc < 0x100000);
    CHECK(freeAfterAlloc > 0);

    // 6. free() must return the space.
    child->free(aligned);
    CHECK(child->getFreeSize() > freeAfterAlloc);

    // 7. Placement new with (heap, align) — the game's operator new overload.
    void* placed = new (child, 0x20) u8[64];
    REQUIRE(placed != nullptr);
    CHECK((reinterpret_cast<uintptr_t>(placed) & 0x1F) == 0);
    child->free(placed);

    // 8. freeAll() resets the heap to a single big block. The theoretical
    // maximum free size is the heap's managed size (mSize) minus one
    // CMemBlock header (0x18 bytes on 64-bit hosts, 0x10 on the original
    // 32-bit target). mSize is not the requested 0x100000: create() carves
    // out sizeof(JKRExpHeap) for the heap object itself.
    child->freeAll();
    CHECK(child->getFreeSize() >= static_cast<s32>(child->mSize - sizeof(JKRExpHeap::CMemBlock)));

    // 9. Global alloc path: JKRHeap::alloc(size, align, heap).
    void* global = JKRHeap::alloc(0x80, 8, child);
    REQUIRE(global != nullptr);
    JKRHeap::free(global, child);

    // 10. Destroy the child heap.
    JKRHeap::destroy(child);

    // 11. Root still usable (allocate directly from it).
    void* fromRoot = root->alloc(16, 4);
    REQUIRE(fromRoot != nullptr);
    root->free(fromRoot);
}

TEST_CASE(jkr_heap_many_blocks) {
    // sRootHeap is typed JKRHeap*; our test heaps are JKRExpHeap (smoke test).
    JKRExpHeap* root = static_cast<JKRExpHeap*>(JKRHeap::sRootHeap);
    REQUIRE(root != nullptr);

    JKRExpHeap* heap = JKRExpHeap::create(0x200000, root, true); // 2 MiB
    REQUIRE(heap != nullptr);

    // Allocate a bunch of blocks of varying sizes, fill them, verify a few.
    constexpr int kBlocks = 256;
    void* blocks[kBlocks] = {};
    for (int i = 0; i < kBlocks; ++i) {
        const size_t size = static_cast<size_t>((i * 7919) % 4096) + 1;
        blocks[i] = heap->alloc(size, 16);
        REQUIRE(blocks[i] != nullptr);
        std::memset(blocks[i], static_cast<int>(i & 0xFF), size);
    }

    // Verify contents survived.
    for (int i = 0; i < kBlocks; ++i) {
        const size_t size = static_cast<size_t>((i * 7919) % 4096) + 1;
        const auto* bytes = static_cast<const uint8_t*>(blocks[i]);
        CHECK_EQ(bytes[0], static_cast<uint8_t>(i & 0xFF));
        CHECK_EQ(bytes[size - 1], static_cast<uint8_t>(i & 0xFF));
    }

    for (void* block : blocks) {
        heap->free(block);
    }

    heap->freeAll();
    JKRHeap::destroy(heap);
}
