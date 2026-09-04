#include "JSystem/JKernel/JKRHeap.hpp"
#include "JSystem/JUtility/JUTException.hpp"
#include <revolution/os/OSBootInfo.h>
#include <new>  // PC_PORT: std::nothrow_t for the nothrow delete overrides
#include "compat/os/OSCompat.h"        // PC_PORT: compat::getBootInfo()
#include "platform/Memory/Memory.h"    // PC_PORT: pre-boot allocator fallback

JKRHeap* JKRHeap::sCurrentHeap;
JKRHeap* JKRHeap::sRootHeap;
JKRHeap* JKRHeap::sSystemHeap;
// PC_PORT (M9.5.1): declared by the header, never defined by the
// decompilation's compiled TUs — the archive stack references it
// (JKRArchive::getFirstFile, JKRDecomp::prepareCommand). Zero-init matches
// the console's BSS; the game heap is created by HeapMemoryWatcher during
// boot, and until then these callers fall back through JKRHeap::alloc's
// nullptr-heap path (sCurrentHeap).
JKRHeap* JKRHeap::sGameHeap;

void* JKRHeap::mCodeStart;
void* JKRHeap::mCodeEnd;
void* JKRHeap::mUserRamStart;
void* JKRHeap::mUserRamEnd;

JKRErrorHandler JKRHeap::mErrorHandler;

static bool byte_806B26D8;
static bool byte_806B70B8;

u32 JKRHeap::mMemorySize;

u32 JKRHeap::ARALT_AramStartAddr = 0x90000000;

JKRHeap::JKRHeap(void* data, u32 size, JKRHeap* parent, bool error) : JKRDisposer(), mChildTree(this), mDisposerList() {
    OSInitMutex(&mMutex);
    mSize = size;
    mStart = (u8*)data;
    mEnd = (u8*)data + size;

    if (parent == nullptr) {
        JKRHeap::sSystemHeap = this;
        JKRHeap::sCurrentHeap = this;
    } else {
        parent->mChildTree.appendChild(&mChildTree);

        if (JKRHeap::sSystemHeap == JKRHeap::sRootHeap) {
            JKRHeap::sSystemHeap = this;
        }

        if (JKRHeap::sCurrentHeap == JKRHeap::sRootHeap) {
            JKRHeap::sCurrentHeap = this;
        }
    }

    mErrorFlag = error;

    if (mErrorFlag == true && mErrorHandler == nullptr) {
        mErrorHandler = JKRDefaultMemoryErrorRoutine;
    }

    _3C = byte_806B26D8;
    _3D = byte_806B70B8;
    _69 = false;
}

JKRHeap::~JKRHeap() {
    mChildTree.getParent()->removeChild(&mChildTree);
    JSUTree< JKRHeap >* nextRootHeap = sRootHeap->mChildTree.getFirstChild();

    if (sCurrentHeap == this)
        sCurrentHeap = !nextRootHeap ? sRootHeap : nextRootHeap->getObject();

    if (sSystemHeap == this)
        sSystemHeap = !nextRootHeap ? sRootHeap : nextRootHeap->getObject();
}

bool JKRHeap::initArena(char** memory, u32* size, int maxHeaps) {
    void *ramStart, *ramEnd, *arenaStart;

    void* arenaLo = OSGetArenaLo();
    void* arenaHi = OSGetArenaHi();

    OSReport("original arenaLo = %p arenaHi = %p\n", arenaLo, arenaHi);

    if (arenaLo == arenaHi) {
        return false;
    }

    arenaStart = OSInitAlloc(arenaLo, arenaHi, maxHeaps);
    // PC_PORT: on the Wii, physical address 0 holds the DOL boot header and
    // OSPhysicalToCached is a macro adding 0x80000000 (PPC address
    // translation). Neither exists on a PC host; compat/os provides the boot
    // header stub instead.
    OSBootInfo* code = (OSBootInfo*)compat::getBootInfo();
    // PC_PORT: upstream aligns with `(u32)ptr & 0xFFFFFFE0` which truncates
    // 64-bit addresses. Use uintptr_t (mask must be 64-bit wide).
    ramStart = (void*)(((uintptr_t)arenaStart + 31) & ~(uintptr_t)31);
    ramEnd = (void*)((uintptr_t)arenaHi & ~(uintptr_t)31);

    JKRHeap::mCodeStart = code;
    JKRHeap::mCodeEnd = ramStart;
    JKRHeap::mUserRamStart = ramStart;
    JKRHeap::mUserRamEnd = ramEnd;
    JKRHeap::mMemorySize = code->memorySize;

    OSSetArenaLo(ramEnd);
    OSSetArenaHi(ramEnd);

    *memory = (char*)ramStart;
    // PC_PORT: pointer difference (fits in u32; the arena is < 4 GiB).
    *size = (u32)((uintptr_t)ramEnd - (uintptr_t)ramStart);
    return true;
}

JKRHeap* JKRHeap::becomeSystemHeap() {
    JKRHeap* sys = sSystemHeap;
    sSystemHeap = this;
    return sys;
}

JKRHeap* JKRHeap::becomeCurrentHeap() {
    JKRHeap* cur = sCurrentHeap;
    sCurrentHeap = this;
    return cur;
}

void JKRHeap::destroy(JKRHeap* pHeap) {
    pHeap->do_destroy();
}

void* JKRHeap::alloc(u32 size, int align, JKRHeap* pHeap) {
    if (pHeap != nullptr) {
        return pHeap->alloc(size, align);
    }

    if (JKRHeap::sCurrentHeap != nullptr) {
        return JKRHeap::sCurrentHeap->alloc(size, align);
    }

    return nullptr;
}

void* JKRHeap::alloc(u32 size, int align) {
    return do_alloc(size, align);
}

void JKRHeap::free(void* pData, JKRHeap* pHeap) {
    if (!pHeap) {
        pHeap = findFromRoot(pData);

        if (!pHeap) {
            return;
        }
    }

    pHeap->do_free(pData);
}

void JKRHeap::free(void* pData) {
    do_free(pData);
}

void JKRHeap::callAllDisposer() {
    while (mDisposerList.mHead != nullptr) {
        reinterpret_cast< JKRDisposer* >(mDisposerList.mHead->mData)->~JKRDisposer();
    }
}

void JKRHeap::freeAll() {
    do_freeAll();
}

void JKRHeap::freeTail() {
    do_freeTail();
}

s32 JKRHeap::resize(void* pData, u32 size) {
    return do_resize(pData, size);
}

s32 JKRHeap::getFreeSize() {
    return do_getFreeSize();
}

void* JKRHeap::getMaxFreeBlock() {
    return do_getMaxFreeBlock();
}

s32 JKRHeap::getTotalFreeSize() {
    return do_getTotalFreeSize();
}

JKRHeap* JKRHeap::findFromRoot(void* pData) {
    JKRHeap* root = sRootHeap;

    if (root == nullptr) {
        return nullptr;
    }

    if ((void*)root->mStart <= pData && pData < (void*)root->mEnd) {
        return root->find(pData);
    }

    return root->findAllHeap(pData);
}

/* functionally equiv but not matching */
JKRHeap* JKRHeap::find(void* pData) const {
    if (mStart <= pData && pData < mEnd) {
        const JSUTree< JKRHeap >& tree = mChildTree;

        if (tree.getNumChildren() != 0) {
            for (JSUTreeIterator< JKRHeap > iterator(mChildTree.getFirstChild()); iterator != mChildTree.getEndChild(); ++iterator) {
                JKRHeap* result = iterator->find(pData);

                if (result) {
                    return result;
                }
            }
        }

        // this is to avoid returning a const JKRHeap ptr
        return const_cast< JKRHeap* >(this);
    }

    return nullptr;
}

/* same here */
JKRHeap* JKRHeap::findAllHeap(void* ptr) const {
    if (mChildTree.getNumChildren() != 0) {
        for (JSUTreeIterator< JKRHeap > iterator(mChildTree.getFirstChild()); iterator != mChildTree.getEndChild(); ++iterator) {
            JKRHeap* heap = iterator->findAllHeap(ptr);

            if (heap != nullptr) {
                return heap;
            }
        }
    }

    if (mStart <= ptr && ptr < mEnd) {
        return const_cast< JKRHeap* >(this);
    }

    return nullptr;
}

// PC_PORT: upstream takes u32 (32-bit addresses); on 64-bit hosts the
// disposer range must use uintptr_t (see patched JKRHeap.hpp).
void JKRHeap::dispose_subroutine(uintptr_t start, uintptr_t end) {
    JSUListIterator< JKRDisposer > last_it;
    JSUListIterator< JKRDisposer > next_it;
    JSUListIterator< JKRDisposer > it;

    for (it = mDisposerList.getFirst(); it != mDisposerList.getEnd(); it = next_it) {
        JKRDisposer* disp = it.getObject();

        if ((void*)start <= disp && disp < (void*)end) {
            disp->~JKRDisposer();

            if (last_it == nullptr) {
                next_it = mDisposerList.getFirst();
            } else {
                next_it = last_it;
                next_it++;
            }
        } else {
            last_it = it;
            next_it = it;
            next_it++;
        }
    }
}

bool JKRHeap::dispose(void* ptr, u32 size) {
    // PC_PORT: upstream truncates to u32 — wrong on 64-bit hosts.
    uintptr_t begin = (uintptr_t)ptr;
    uintptr_t end = (uintptr_t)ptr + size;
    dispose_subroutine(begin, end);
    return false;
}

void JKRHeap::dispose(void* begin, void* end) {
    dispose_subroutine((uintptr_t)begin, (uintptr_t)end);
}

void JKRHeap::dispose() {
    const JSUList< JKRDisposer >& list = mDisposerList;
    JSUListIterator< JKRDisposer > iterator;

    while (list.getFirst() != list.getEnd()) {
        iterator = list.getFirst();
        iterator->~JKRDisposer();
    }
}

void JKRHeap::copyMemory(void* pDst, void* pSrc, u32 size) {
    u32 count = (size + 3) / 4;
    u32* dst_32 = (u32*)pDst;
    u32* src_32 = (u32*)pSrc;

    while (count > 0) {
        *dst_32 = *src_32;
        dst_32++;
        src_32++;
        count--;
    }
}

void JKRDefaultMemoryErrorRoutine(void* pHeap, u32 size, int alignment) {
    JUTException::panic_f(__FILE__, 0x355, "%s", "abort\n");
}

JKRErrorHandler JKRHeap::setErrorHandler(JKRErrorHandler errorHandler) {
    JKRErrorHandler prev = JKRHeap::mErrorHandler;

    if (!errorHandler) {
        errorHandler = JKRDefaultMemoryErrorRoutine;
    }

    JKRHeap::mErrorHandler = errorHandler;
    return prev;
}

// =============================================================================
// PC_PORT — global operator new/delete.
//
// On the Wii, the game IS the whole process: every C++ allocation (including
// the C++ runtime's own) goes through the JKR heap hierarchy, and the root
// heap exists before any game code runs. On a PC host the process runtime
// (iostreams, std::vector in static storage, test registries, ...) allocates
// BEFORE the game boot creates the JKR root heap (and even before main()).
//
// Strategy: while `JKRHeap::sRootHeap` is null (pre-boot), route through
// Platform::Memory (malloc-backed, aligned). Once the game boot creates the
// root heap, allocation matches the console: everything goes through JKR.
// operator delete resolves the pointer's owning heap via findFromRoot() so
// pre-boot (platform) allocations and post-boot (JKR) allocations are both
// freed correctly even if their lifetimes cross the boot boundary.
//
// The placement forms (`new (heap, align)`) keep their original semantics:
// the explicit-heap forms always use that heap; the `new (align)` form uses
// the current heap when available, falling back to Platform::Memory pre-boot.
//
// PC_PORT alignment: the console's plain `new` went through JKR with 4-byte
// alignment, which is fine on PPC (no strict alignment traps). x86-64 code —
// including the host runtime and dlopen'd libraries (SDL, lavapipe/LLVM, ...)
// — requires at least alignof(max_align_t) = 16 for plain new, and misaligned
// stores fault (movaps). We therefore route through JKR with 16-byte
// alignment for the plain forms. Game code that needs more alignment already
// uses the explicit forms (`new (align)`, `new (heap, align)`), so game
// behavior is unchanged. TODO(PC_PORT): a library that fills the JKR arena
// (or requests exotic alignment) needs a dedicated allocation policy in M5+.
// =============================================================================
constexpr size_t kPcNewAlign = 16;

void* operator new(size_t size) {
    // PC_PORT (M9.5.3d): the SYSTEM allocator, never the current JKR heap.
    // Plain new used to route to sCurrentHeap = the SCENE heap during a
    // scene; HeapMemoryWatcher destroys those heaps at every scene transition
    // (the first ever is Logo->Title), which freed every std:: container that
    // allocated through plain new platform-wide (Renderer pipeline/sampler
    // caches, GXCompat tables...) under their owners — dangling nodes, the
    // random-SEGV/"Bad Block" family. Heap-pinned game allocations use the
    // explicit new (heap, align) placement forms; pcPortFree pairs with this
    // (findFromRoot falls through to the platform allocator).
    return Platform::Memory::allocate(size, kPcNewAlign);
}

void* operator new(size_t size, int align) {
    // PC_PORT (M9.5.3d): the SYSTEM allocator, never the current JKR heap.
    // Plain new used to route to sCurrentHeap = the SCENE heap during a
    // scene; HeapMemoryWatcher destroys those heaps at every scene transition
    // (the first ever is Logo->Title), which freed every std:: container that
    // allocated through plain new platform-wide (Renderer pipeline/sampler
    // caches, GXCompat tables...) under their owners — dangling nodes, the
    // random-SEGV/"Bad Block" family. Heap-pinned game allocations use the
    // explicit new (heap, align) placement forms; pcPortFree pairs with this
    // (findFromRoot falls through to the platform allocator).
    return Platform::Memory::allocate(size, static_cast<size_t>(align));
}

void* operator new(size_t size, const std::nothrow_t&) noexcept {
    // PC_PORT (M9.5.3d): pair for the nothrow/sized deletes below. Third-
    // party code in our process (lavapipe's LLVM JIT compiles shaders at
    // runtime) allocates with nothrow new; without this override those blocks
    // came from the SYSTEM allocator while their deletes went through
    // pcPortFree (Platform::Memory::free on a foreign pointer). ASAN flagged
    // the heap-buffer-overflow; non-ASAN builds corrupted whatever malloc
    // reused the block (the renderer's pipeline cache died at the first
    // scene transition). Routing to the platform allocator pairs it with
    // pcPortFree's fallback.
    return Platform::Memory::allocate(size, kPcNewAlign);
}

void* operator new(size_t size, JKRHeap* pHeap, int align) {
    // Always a JKR heap: this is the game's placement-new with explicit heap.
    return JKRHeap::alloc(static_cast<u32>(size), align, pHeap);
}

void* operator new[](size_t size) {
    // PC_PORT (M9.5.3d): the SYSTEM allocator, never the current JKR heap.
    // Plain new used to route to sCurrentHeap = the SCENE heap during a
    // scene; HeapMemoryWatcher destroys those heaps at every scene transition
    // (the first ever is Logo->Title), which freed every std:: container that
    // allocated through plain new platform-wide (Renderer pipeline/sampler
    // caches, GXCompat tables...) under their owners — dangling nodes, the
    // random-SEGV/"Bad Block" family. Heap-pinned game allocations use the
    // explicit new (heap, align) placement forms; pcPortFree pairs with this
    // (findFromRoot falls through to the platform allocator).
    return Platform::Memory::allocate(size, kPcNewAlign);
}

void* operator new[](size_t size, int align) {
    // PC_PORT (M9.5.3d): the SYSTEM allocator, never the current JKR heap.
    // Plain new used to route to sCurrentHeap = the SCENE heap during a
    // scene; HeapMemoryWatcher destroys those heaps at every scene transition
    // (the first ever is Logo->Title), which freed every std:: container that
    // allocated through plain new platform-wide (Renderer pipeline/sampler
    // caches, GXCompat tables...) under their owners — dangling nodes, the
    // random-SEGV/"Bad Block" family. Heap-pinned game allocations use the
    // explicit new (heap, align) placement forms; pcPortFree pairs with this
    // (findFromRoot falls through to the platform allocator).
    return Platform::Memory::allocate(size, static_cast<size_t>(align));
}

void* operator new[](size_t size, const std::nothrow_t&) noexcept {
    // PC_PORT (M9.5.3d): nothrow pair — see operator new(nothrow_t).
    return Platform::Memory::allocate(size, kPcNewAlign);
}

void* operator new[](size_t size, JKRHeap* pHeap, int align) {
    // Always a JKR heap (explicit heap placement-new).
    return JKRHeap::alloc(static_cast<u32>(size), align, pHeap);
}

// PC_PORT: resolve the owning heap for the pointer; fall back to the platform
// allocator for anything that is not inside a JKR heap (pre-boot allocations,
// or pointers allocated before the root heap existed).
static void pcPortFree(void* pData) {
    if (pData == nullptr) {
        return;
    }
    JKRHeap* heap = JKRHeap::sRootHeap ? JKRHeap::findFromRoot(pData) : nullptr;
    if (heap != nullptr) {
        heap->do_free(pData);
    } else {
        Platform::Memory::free(pData);
    }
}

void operator delete(void* pData) {
    pcPortFree(pData);
}

void operator delete[](void* pData) {
    pcPortFree(pData);
}

// PC_PORT (M9.5.3a, Windows fix): sized deallocation overrides.
//
// MSVC (C++14+) emits calls to the SIZED forms `operator delete(void*, size_t)`
// by default (/Zc:sizedDealloc is on), while GCC leaves -fsized-deallocation
// off. Without these overrides, MSVC routed every plain `delete` of a
// JKR-heap-allocated object (placement-new'd via `new (heap, align)`, or
// allocated by the global operator new overrides above) straight into the
// CRT's free() -> invalid free, heap corruption, intermittent 0xC0000005 and
// the HeapMemoryWatcher alloc-failure panic in the full test suite. Both sized
// forms simply forward to pcPortFree (the size hint is ignored: pcPortFree
// resolves the owning JKR heap via findFromRoot, or falls back to the
// platform allocator for pre-boot pointers). The nothrow delete forms are
// provided for the same reason.
void operator delete(void* pData, size_t) {
    pcPortFree(pData);
}

void operator delete[](void* pData, size_t) {
    pcPortFree(pData);
}

void operator delete(void* pData, const std::nothrow_t&) noexcept {
    pcPortFree(pData);
}

void operator delete[](void* pData, const std::nothrow_t&) noexcept {
    pcPortFree(pData);
}

// PC_PORT (M9.5.3d): sized nothrow deletes (C++17; emitted when a nothrow-new
// allocation is deleted with a size hint). Same routing as the sized forms.
void operator delete(void* pData, size_t, const std::nothrow_t&) noexcept {
    pcPortFree(pData);
}

void operator delete[](void* pData, size_t, const std::nothrow_t&) noexcept {
    pcPortFree(pData);
}

void JKRHeap::state_register(TState*, u32) const {
    return;
}

bool JKRHeap::state_compare(const TState& lhs, const TState& rhs) const {
    return lhs.mCheckCode == rhs.mCheckCode;
}

void JKRHeap::state_dump(const TState&) const {
    return;
}

void JKRHeap::setAltAramStartAdr(u32 addr) {
    ARALT_AramStartAddr = addr;
}

u32 JKRHeap::getAltAramStartAdr() {
    return ARALT_AramStartAddr;
}

s32 JKRHeap::do_changeGroupID(u8) {
    return 0;
}

u8 JKRHeap::do_getCurrentGroupId() {
    return 0;
}

bool JKRHeap::dump_sort() {
    return true;
}
