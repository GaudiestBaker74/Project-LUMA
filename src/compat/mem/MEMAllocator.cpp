// =============================================================================
// compat/mem — MEMAllocator forwarders (M9.5.2).
//
// Host implementation of the RVL mem/allocator.c entry points. The vendored
// file is not compiled: it includes the RVL expHeap header tree (which the
// compat include overrides do not model), and MEMInitAllocatorForExpHeap is
// unreachable on the host — the game and nw4r always install their own
// MEMAllocatorFunc tables backed by JKRHeap (MR::JKRHeapAllocator) or by
// operator new/delete (MR::NewDeleteAllocator, used by the nw4r layout
// engine through Layout::mspAllocator).
//
// MEMAllocFromAllocator / MEMFreeToAllocator are byte-for-byte the vendored
// forwarders. MEMInitAllocatorForExpHeap is a loud no-op (link-time safety
// net; reaching it at runtime would mean an unported code path).
// =============================================================================
#include <revolution/mem/allocator.h>

#include "platform/Log/Log.h"

extern "C" {

void* MEMAllocFromAllocator(MEMAllocator* pAllocator, u32 size) {
    return (*pAllocator->pFunc->pfAlloc)(pAllocator, size);
}

void MEMFreeToAllocator(MEMAllocator* pAllocator, void* pBlock) {
    (*pAllocator->pFunc->pfFree)(pAllocator, pBlock);
}

void MEMInitAllocatorForExpHeap(MEMAllocator* /*pAllocator*/, MEMHeapHandle /*handle*/,
                                int /*align*/) {
    // MEMExpHeap is not ported (the host uses JKRHeap arenas). Fail loudly.
    PL_LOG_ERROR("compat.mem", "MEMInitAllocatorForExpHeap: MEM ExpHeaps are not ported");
}

} // extern "C"
