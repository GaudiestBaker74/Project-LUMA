#include "tests/test_runner.h"

#include "platform/Memory/Memory.h"

#include <cstdint>
#include <cstring>

TEST_CASE(memory_alloc_free) {
    const auto before = Platform::Memory::stats();

    void* p = Platform::Memory::allocate(256);
    REQUIRE(p != nullptr);
    CHECK_EQ(Platform::Memory::sizeOf(p), static_cast<size_t>(256));
    CHECK_EQ(Platform::Memory::stats().allocationCount, before.allocationCount + 1);

    Platform::Memory::free(p);
    CHECK_EQ(Platform::Memory::stats().allocationCount, before.allocationCount);
}

TEST_CASE(memory_alignment) {
    const size_t alignments[] = {1, 2, 4, 8, 16, 64, 256, 4096};
    for (size_t alignment : alignments) {
        void* p = Platform::Memory::allocate(123, alignment);
        REQUIRE(p != nullptr);
        CHECK(Platform::Memory::isAligned(reinterpret_cast<uintptr_t>(p), alignment));
        Platform::Memory::free(p);
    }
}

TEST_CASE(memory_zero_init) {
    void* p = Platform::Memory::allocate(64);
    REQUIRE(p != nullptr);
    const auto* bytes = static_cast<const uint8_t*>(p);
    for (int i = 0; i < 64; ++i) {
        CHECK_EQ(bytes[i], 0);
    }
    Platform::Memory::free(p);
}

TEST_CASE(memory_reallocate) {
    void* p = Platform::Memory::allocate(16);
    REQUIRE(p != nullptr);
    std::memset(p, 0x5A, 16);

    void* q = Platform::Memory::reallocate(p, 256);
    REQUIRE(q != nullptr);
    CHECK_EQ(Platform::Memory::sizeOf(q), static_cast<size_t>(256));
    // First 16 bytes must be preserved.
    const auto* bytes = static_cast<const uint8_t*>(q);
    for (int i = 0; i < 16; ++i) {
        CHECK_EQ(bytes[i], 0x5A);
    }
    Platform::Memory::free(q);
}

TEST_CASE(memory_edge_cases) {
    CHECK(Platform::Memory::allocate(0) == nullptr);
    Platform::Memory::free(nullptr); // must not crash
    CHECK(Platform::Memory::allocate(10, 3) == nullptr); // non-power-of-two

    const auto before = Platform::Memory::stats();
    Platform::Memory::free(Platform::Memory::allocate(8));
    CHECK_EQ(Platform::Memory::stats().allocationCount, before.allocationCount);
}

TEST_CASE(memory_peak_tracking) {
    const auto before = Platform::Memory::stats();
    void* a = Platform::Memory::allocate(1024);
    void* b = Platform::Memory::allocate(2048);
    REQUIRE(a && b);

    const auto mid = Platform::Memory::stats();
    // Outstanding bytes grew by the two allocations (the test runner itself
    // may have transient allocations between snapshots, so only require >=).
    CHECK(mid.bytesAllocated >= before.bytesAllocated + 3072);
    // The peak is a high-water mark: it must be >= the current outstanding
    // bytes and never lower than the previous peak.
    CHECK(mid.bytesPeak >= mid.bytesAllocated);
    CHECK(mid.bytesPeak >= before.bytesPeak);

    Platform::Memory::free(a);
    Platform::Memory::free(b);
    const auto after = Platform::Memory::stats();
    CHECK(after.bytesAllocated <= before.bytesAllocated);
    CHECK(after.bytesPeak == mid.bytesPeak);
}

TEST_CASE(memory_align_helpers) {
    CHECK_EQ(Platform::Memory::alignUp(1, 16), static_cast<size_t>(16));
    CHECK_EQ(Platform::Memory::alignUp(16, 16), static_cast<size_t>(16));
    CHECK_EQ(Platform::Memory::alignUp(17, 16), static_cast<size_t>(32));
    CHECK_EQ(Platform::Memory::alignUp(0, 16), static_cast<size_t>(0));
    CHECK(Platform::Memory::isAligned(16, 16));
    CHECK(!Platform::Memory::isAligned(17, 16));
}

TEST_CASE(memory_many_allocations) {
    // A quick stress: interleave allocations and frees, verify the bookkeeping
    // returns to zero at the end.
    const auto before = Platform::Memory::stats();
    void* blocks[64] = {};
    for (int round = 0; round < 4; ++round) {
        for (int i = 0; i < 64; ++i) {
            blocks[i] = Platform::Memory::allocate(static_cast<size_t>(i * 37 + 1));
            REQUIRE(blocks[i] != nullptr);
        }
        for (int i = 0; i < 64; ++i) {
            Platform::Memory::free(blocks[i]);
            blocks[i] = nullptr;
        }
    }
    CHECK_EQ(Platform::Memory::stats().allocationCount, before.allocationCount);
    CHECK_EQ(Platform::Memory::stats().bytesAllocated, before.bytesAllocated);
}
