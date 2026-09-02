#include "JSystem/JKernel/JKRExpHeap.hpp"
#include "JSystem/JUtility/JUTConsole.hpp"
#include <new>

static u32 DBfoundSize;
static u32 DBfoundOffset;
static JKRExpHeap::CMemBlock* DBfoundBlock;
static JKRExpHeap::CMemBlock* DBnewFreeBlock;
static JKRExpHeap::CMemBlock* DBnewUsedBlock;

JKRExpHeap* JKRExpHeap::createRoot(int heapNum, bool a2) {
    JKRExpHeap* heap = nullptr;

    if (!JKRHeap::sRootHeap) {
        char* stack_C;
        u32 arenaSize;
        JKRHeap::initArena(&stack_C, &arenaSize, heapNum);
        // PC_PORT: the object is 0x100 bytes on 64-bit hosts (see JKRExpHeap
        // layout); the PPC32 size is 0x90. Using sizeof keeps the header
        // reservation correct on every platform. Upstream hardcodes 0x90.
        const u32 objSize = ALIGN_NEXT(sizeof(JKRExpHeap), 0x10);
        char* area = stack_C + objSize;
        u32 size = arenaSize - objSize;
        heap = new (stack_C) JKRExpHeap(area, size, nullptr, a2);
        JKRHeap::sRootHeap = heap;
    } else {
        // PC_PORT: the console calls createRoot exactly once (the gameMain
        // boot); the upstream decomp leaves `heap` null on a second call and
        // crashes writing mAllocMode. On the host the root heap is a
        // process-wide singleton shared by the test suite (jkr_heap_test
        // creates it first, the M9 boot tests reuse it) — return the existing
        // root instead of dereferencing null.
        heap = static_cast<JKRExpHeap*>(JKRHeap::sRootHeap);
    }

    heap->mAllocMode = 1;
    return heap;
}

JKRExpHeap* JKRExpHeap::create(u32 size, JKRHeap* pParent, bool errorFlag) {
    if (!pParent) {
        pParent = JKRHeap::sRootHeap;
    }

    if (size == 0xFFFFFFFF) {
        size = pParent->getMaxAllocatableSize(0x10);
    }

    u32 alignedSize = ALIGN_PREV(size, 0x10);
    u32 heapSize = ALIGN_NEXT(sizeof(JKRExpHeap), 0x10);

    if (alignedSize < 0xA0) {
        return nullptr;
    }

    u8* mem = (u8*)JKRHeap::alloc(alignedSize, 16, pParent);
    u8* data = (mem + heapSize);
    if (mem == nullptr) {
        return nullptr;
    }

    JKRExpHeap* heap = new (mem) JKRExpHeap(data, alignedSize - heapSize, pParent, errorFlag);

    if (heap == nullptr) {
        JKRHeap::free(mem, nullptr);
        return nullptr;
    }

    heap->mAllocMode = 0;
    return heap;
}

JKRExpHeap* JKRExpHeap::create(void* ptr, u32 size, JKRHeap* pParent, bool errorFlag) {
    JKRHeap* parent;

    if (pParent == nullptr) {
        parent = sRootHeap->find(ptr);

        if (parent == nullptr) {
            return nullptr;
        }
    } else {
        parent = pParent;
    }

    JKRExpHeap* heap = nullptr;
    u32 heapSize = ALIGN_NEXT(sizeof(JKRExpHeap), 0x10);

    if (size < heapSize) {
        return nullptr;
    }

    void* data = (u8*)ptr + heapSize;
    // PC_PORT: upstream uses (u32) pointer casts (PPC32). On 64-bit hosts the
    // byte distance between two pointers in the same heap must be computed via
    // uintptr_t; the result is small and fits in u32.
    u32 alignSize = (u32)ALIGN_PREV((uintptr_t)ptr + size - (uintptr_t)data, 0x10);
    if (ptr != nullptr) {
        heap = new (ptr) JKRExpHeap(data, alignSize, parent, errorFlag);
    }

    heap->mAllocMode = 1;
    heap->_70 = ptr;
    heap->_74 = size;
    return heap;
}

void JKRExpHeap::do_destroy() {
    if (_6E) {
        JKRHeap* heap = mChildTree.getParent()->getObject();

        if (heap != nullptr) {
            this->~JKRExpHeap();
            JKRHeap::free(this, heap);
        }
    } else {
        this->~JKRExpHeap();
    }
}

void* JKRExpHeap::do_alloc(u32 size, int align) {
    void* ptr;
    OSLockMutex(&mMutex);

    if (size < 4) {
        size = 4;
    }

    if (align >= 0) {
        if (align <= 4) {
            ptr = allocFromHead(size);
        } else {
            ptr = allocFromHead(size, align);
        }
    } else {
        if (-align <= 4) {
            ptr = allocFromTail(size);
        } else {
            ptr = allocFromTail(size, -align);
        }
    }

    if (ptr == nullptr) {
        JUTWarningConsole_f(":::cannot alloc memory (0x%x byte).\n", size);

        if (JKRHeap::mErrorFlag == true) {
            if (JKRHeap::mErrorHandler) {
                (*JKRHeap::mErrorHandler)(this, size, align);
            }
        }
    }

    OSUnlockMutex(&mMutex);
    return ptr;
}

// JKRExpheap::allocFromHead
// JKRExpHeap::allocFromTail

JKRExpHeap::JKRExpHeap(void* data, u32 size, JKRHeap* parent, bool error) : JKRHeap(data, size, parent, error) {
    CMemBlock* block = (CMemBlock*)data;

    _6A = 0;
    _6B = 0xFF;
    mHeadFreeList = block;
    mTailFreeList = block;
    block->initiate(nullptr, nullptr, size - sizeof(CMemBlock), 0, 0);
    mHeadUsedList = nullptr;
    mTailUsedList = nullptr;
}

void JKRExpHeap::CMemBlock::initiate(CMemBlock* prev, CMemBlock* next, u32 size, u8 groupID, u8 align) {
    mMagic = 'HM';
    mFlags = align;
    mGroupId = groupID;
    mSize = size;
    mPrev = prev;
    mNext = next;
}

JKRExpHeap::CMemBlock* JKRExpHeap::CMemBlock::allocFore(u32 size, u8 group_1, u8 align_1, u8 group_2, u8 align_2) {
    CMemBlock* block = nullptr;
    mGroupId = group_1;
    mFlags = align_1;

    if (mSize >= size + sizeof(CMemBlock)) {
        // PC_PORT: upstream adds `size` to `this` as u32 (PPC32) — truncates
        // 64-bit addresses.
        block = (CMemBlock*)((uintptr_t)this + size);
        block[1].mGroupId = group_2;
        block[1].mFlags = align_2;
        block[1].mSize = mSize - (size + sizeof(CMemBlock));
        mSize = size;
        block++;
    }

    return block;
}

JKRExpHeap::CMemBlock* JKRExpHeap::CMemBlock::allocBack(u32 size, u8 group_1, u8 align_1, u8 group_2, u8 align_2) {
    CMemBlock* block = nullptr;

    if (mSize >= size + sizeof(CMemBlock)) {
        // PC_PORT: see the allocFore cast — uintptr_t for 64-bit hosts.
        block = (CMemBlock*)((uintptr_t)this + mSize - size);
        block->mGroupId = group_2;
        block->mFlags = align_2 | 0x80;
        block->mSize = size;
        mGroupId = group_1;
        mFlags = align_1;
        mSize -= size + sizeof(CMemBlock);
    } else {
        mGroupId = group_2;
        mFlags = 0x80;
    }

    return block;
}

JKRExpHeap::CMemBlock* JKRExpHeap::CMemBlock::getHeapBlock(void* ptr) {
    if (ptr != nullptr) {
        CMemBlock* block = (CMemBlock*)ptr - 1;

        if (block->mMagic == 'HM') {
            return block;
        }
    }

    return nullptr;
}

// ============================================================================
// PC_PORT — the rest of JKRExpHeap.
//
// Upstream Petari (9ecdff2) has only decompiled the first ~160 lines of this
// file; the free-list allocator core below is declared in JKRExpHeap.hpp but
// has no definition in the repository yet. These are functionally-equivalent
// implementations of the same JSystem library class (layout-identical), based
// on the canonical JKRExpHeap algorithm as verified in other Nintendo
// decompilations (e.g. the Wind Waker decomp, same class/layout).
//
// Replace with upstream decompiled code when Petari publishes it.
// TODO(PC_PORT): verify against upstream JKRExpHeap.cpp when decompiled.
// ============================================================================

JKRExpHeap::~JKRExpHeap() {
    dispose();
}

u32 JKRExpHeap::getHeapType() {
    return 'EXPH';
}

void* JKRExpHeap::allocFromHead(u32 size, int align) {
    size = ALIGN_NEXT(size, 4);

    s32 foundSize = -1;
    u32 foundOffset = 0;
    CMemBlock* foundBlock = nullptr;
    CMemBlock* newFreeBlock = nullptr;
    CMemBlock* newUsedBlock = nullptr;

    for (CMemBlock* block = mHeadFreeList; block; block = block->mNext) {
        uintptr_t alignedContent = ALIGN_NEXT((uintptr_t)block->getContent(), align);
        u32 offset = (u32)(alignedContent - (uintptr_t)block->getContent());

        if (block->mSize < size + offset) {
            continue;
        }

        // PC_PORT: keep the original s32-vs-u32 comparison semantics. Casting
        // block->mSize to s32 would make foundSize(-1) <= positive always
        // true and skip the first (and every) block. With u32 arithmetic the
        // sentinel -1 becomes 0xFFFFFFFF and never matches.
        if (foundSize <= block->mSize) {
            continue;
        }

        foundSize = block->mSize;
        foundBlock = block;
        foundOffset = offset;

        if (mAllocMode != 0) {
            break;
        }

        if (block->mSize == size) {
            break;
        }
    }

    DBfoundSize = foundSize;
    DBfoundOffset = foundOffset;
    DBfoundBlock = foundBlock;

    if (foundBlock) {
        if (foundOffset >= sizeof(CMemBlock)) {
            CMemBlock* prev = foundBlock->mPrev;
            CMemBlock* next = foundBlock->mNext;
            newUsedBlock = foundBlock->allocFore(foundOffset - sizeof(CMemBlock), 0, 0, 0, 0);

            if (newUsedBlock) {
                newFreeBlock = newUsedBlock->allocFore(size, mCurrentGroupId, 0, 0, 0);
            } else {
                newFreeBlock = nullptr;
            }

            if (newFreeBlock) {
                setFreeBlock(foundBlock, prev, newFreeBlock);
            } else {
                setFreeBlock(foundBlock, prev, next);
            }

            if (newFreeBlock) {
                setFreeBlock(newFreeBlock, foundBlock, next);
            }

            appendUsedList(newUsedBlock);
            DBnewFreeBlock = newFreeBlock;
            DBnewUsedBlock = newUsedBlock;
            return newUsedBlock->getContent();
        } else {
            if (foundOffset != 0) {
                CMemBlock* prev = foundBlock->mPrev;
                CMemBlock* next = foundBlock->mNext;
                removeFreeBlock(foundBlock);
                newUsedBlock = (CMemBlock*)((uintptr_t)foundBlock + foundOffset);
                newUsedBlock->mSize = foundBlock->mSize - foundOffset;
                newFreeBlock =
                    newUsedBlock->allocFore(size, mCurrentGroupId, (u8)foundOffset, 0, 0);

                if (newFreeBlock) {
                    setFreeBlock(newFreeBlock, prev, next);
                }

                appendUsedList(newUsedBlock);
                return newUsedBlock->getContent();
            } else {
                CMemBlock* prev = foundBlock->mPrev;
                CMemBlock* next = foundBlock->mNext;
                newFreeBlock = foundBlock->allocFore(size, mCurrentGroupId, 0, 0, 0);
                removeFreeBlock(foundBlock);

                if (newFreeBlock) {
                    setFreeBlock(newFreeBlock, prev, next);
                }

                appendUsedList(foundBlock);
                return foundBlock->getContent();
            }
        }
    }

    return nullptr;
}

void* JKRExpHeap::allocFromHead(u32 size) {
    size = ALIGN_NEXT(size, 4);

    s32 foundSize = -1;
    CMemBlock* foundBlock = nullptr;

    for (CMemBlock* block = mHeadFreeList; block; block = block->mNext) {
        if (block->mSize < size) {
            continue;
        }

        // PC_PORT: see allocFromHead(u32, int) — keep u32 comparison semantics.
        if (foundSize <= block->mSize) {
            continue;
        }

        foundSize = block->mSize;
        foundBlock = block;

        if (mAllocMode != 0) {
            break;
        }

        if (foundSize == (s32)size) {
            break;
        }
    }

    if (foundBlock) {
        CMemBlock* newblock = foundBlock->allocFore(size, mCurrentGroupId, 0, 0, 0);

        if (newblock) {
            setFreeBlock(newblock, foundBlock->mPrev, foundBlock->mNext);
        } else {
            removeFreeBlock(foundBlock);
        }

        appendUsedList(foundBlock);
        return foundBlock->getContent();
    }

    return nullptr;
}

void* JKRExpHeap::allocFromTail(u32 size, int align) {
    u32 offset = 0;
    CMemBlock* foundBlock = nullptr;
    CMemBlock* newBlock = nullptr;
    u32 usedSize = 0;
    uintptr_t start = 0;

    for (CMemBlock* block = mTailFreeList; block; block = block->mPrev) {
        start = ALIGN_PREV((uintptr_t)block->getContent() + block->mSize - size, align);
        usedSize = (u32)((uintptr_t)block->getContent() + block->mSize - start);

        if (block->mSize >= usedSize) {
            foundBlock = block;
            offset = block->mSize - usedSize;
            newBlock = (CMemBlock*)start - 1;
            break;
        }
    }

    if (foundBlock != nullptr) {
        if (offset >= sizeof(CMemBlock)) {
            newBlock->initiate(nullptr, nullptr, usedSize, mCurrentGroupId, 0x80);
            foundBlock->mSize = foundBlock->mSize - usedSize - sizeof(CMemBlock);
            appendUsedList(newBlock);
            return newBlock->getContent();
        } else {
            if (offset != 0) {
                removeFreeBlock(foundBlock);
                newBlock->initiate(nullptr, nullptr, usedSize, mCurrentGroupId, offset | 0x80);
                appendUsedList(newBlock);
                return newBlock->getContent();
            } else {
                removeFreeBlock(foundBlock);
                newBlock->initiate(nullptr, nullptr, usedSize, mCurrentGroupId, 0x80);
                appendUsedList(newBlock);
                return newBlock->getContent();
            }
        }
    }

    return nullptr;
}

void* JKRExpHeap::allocFromTail(u32 size) {
    u32 size2 = ALIGN_NEXT(size, 4);
    CMemBlock* foundBlock = nullptr;

    for (CMemBlock* block = mTailFreeList; block; block = block->mPrev) {
        if (block->mSize >= size2) {
            foundBlock = block;
            break;
        }
    }

    if (foundBlock != nullptr) {
        CMemBlock* usedBlock = foundBlock->allocBack(size2, 0, 0, mCurrentGroupId, 0);
        CMemBlock* freeBlock = nullptr;

        if (usedBlock) {
            freeBlock = foundBlock;
        } else {
            removeFreeBlock(foundBlock);
            usedBlock = foundBlock;
            freeBlock = nullptr;
        }

        if (freeBlock) {
            setFreeBlock(freeBlock, foundBlock->mPrev, foundBlock->mNext);
        }

        appendUsedList(usedBlock);
        return usedBlock->getContent();
    }

    return nullptr;
}

void JKRExpHeap::do_free(void* ptr) {
    lock();

    if ((uintptr_t)getStartAddr() <= (uintptr_t)ptr && (uintptr_t)ptr <= (uintptr_t)getEndAddr()) {
        CMemBlock* block = CMemBlock::getHeapBlock(ptr);

        if (block) {
            removeUsedBlock(block);
            recycleFreeBlock(block);
        }
    } else {
        JUTWarningConsole_f("free: memblock %x not in heap %x\n", ptr, this);
    }

    unlock();
}

void JKRExpHeap::do_freeAll() {
    lock();
    callAllDisposer();
    mHeadFreeList = (CMemBlock*)getStartAddr();
    mTailFreeList = mHeadFreeList;
    mHeadFreeList->initiate(nullptr, nullptr, mSize - sizeof(CMemBlock), 0, 0);
    mHeadUsedList = nullptr;
    mTailUsedList = nullptr;
    unlock();
}

void JKRExpHeap::do_freeTail() {
    lock();

    for (CMemBlock* block = mHeadUsedList; block != nullptr;) {
        if ((block->mFlags & 0x80) != 0) {
            dispose(block + 1, block->mSize);
            CMemBlock* temp = block->mNext;
            removeUsedBlock(block);
            recycleFreeBlock(block);
            block = temp;
        } else {
            block = block->mNext;
        }
    }

    unlock();
}

void JKRExpHeap::do_fillFreeArea() {}

s32 JKRExpHeap::do_resize(void* ptr, u32 size) {
    lock();
    CMemBlock* block = CMemBlock::getHeapBlock(ptr);

    if (block == nullptr || (uintptr_t)ptr < (uintptr_t)mStart || (uintptr_t)mEnd < (uintptr_t)ptr) {
        unlock();
        return -1;
    }

    size = ALIGN_NEXT(size, 4);

    if (size == block->mSize) {
        unlock();
        return size;
    }

    if (size > block->mSize) {
        CMemBlock* foundBlock = nullptr;

        for (CMemBlock* freeBlock = mHeadFreeList; freeBlock; freeBlock = freeBlock->mNext) {
            if (freeBlock == (CMemBlock*)((uintptr_t)(block + 1) + block->mSize)) {
                foundBlock = freeBlock;
                break;
            }
        }

        if (foundBlock == nullptr) {
            unlock();
            return -1;
        }

        if (size > block->mSize + sizeof(CMemBlock) + foundBlock->mSize) {
            unlock();
            return -1;
        }

        removeFreeBlock(foundBlock);
        block->mSize += foundBlock->mSize + sizeof(CMemBlock);

        if (block->mSize - size > sizeof(CMemBlock)) {
            CMemBlock* newBlock = block->allocFore(size, block->mGroupId, block->mFlags, 0, 0);

            if (newBlock) {
                recycleFreeBlock(newBlock);
            }
        }
    } else {
        if (block->mSize - size > sizeof(CMemBlock)) {
            CMemBlock* freeBlock = block->allocFore(size, block->mGroupId, block->mFlags, 0, 0);

            if (freeBlock) {
                recycleFreeBlock(freeBlock);
            }
        }
    }

    unlock();
    return block->mSize;
}

s32 JKRExpHeap::do_getSize(void* ptr) {
    lock();
    CMemBlock* block = CMemBlock::getHeapBlock(ptr);

    if (!block || (uintptr_t)ptr < (uintptr_t)getStartAddr() || (uintptr_t)getEndAddr() < (uintptr_t)ptr) {
        unlock();
        return -1;
    }

    unlock();
    return block->mSize;
}

s32 JKRExpHeap::do_getFreeSize() {
    lock();
    s32 size = 0;

    for (CMemBlock* block = mHeadFreeList; block; block = block->mNext) {
        if (size < (s32)block->mSize) {
            size = block->mSize;
        }
    }

    unlock();
    return size;
}

void* JKRExpHeap::do_getMaxFreeBlock() {
    lock();
    s32 size = 0;
    CMemBlock* res = nullptr;

    for (CMemBlock* block = mHeadFreeList; block; block = block->mNext) {
        if (size < (s32)block->mSize) {
            size = block->mSize;
            res = block;
        }
    }

    unlock();
    return res;
}

s32 JKRExpHeap::do_getTotalFreeSize() {
    u32 size = 0;
    lock();

    for (CMemBlock* block = mHeadFreeList; block; block = block->mNext) {
        size += block->mSize;
    }

    unlock();
    return size;
}

s32 JKRExpHeap::do_changeGroupID(u8 groupId) {
    lock();
    u8 prev = mCurrentGroupId;
    mCurrentGroupId = groupId;
    unlock();
    return prev;
}

u8 JKRExpHeap::do_getCurrentGroupId() {
    return mCurrentGroupId;
}

void JKRExpHeap::appendUsedList(CMemBlock* newblock) {
    if (!newblock) {
        OSPanic(__FILE__, 0, ":::ERROR! appendUsedList\n");
    }

    CMemBlock* block = mTailUsedList;
    newblock->mMagic = 'HM';

    if (block) {
        block->mNext = newblock;
        newblock->mPrev = block;
    } else {
        newblock->mPrev = nullptr;
    }

    mTailUsedList = newblock;

    if (!mHeadUsedList) {
        mHeadUsedList = newblock;
    }

    newblock->mNext = nullptr;
}

void JKRExpHeap::setFreeBlock(CMemBlock* block, CMemBlock* prev, CMemBlock* next) {
    if (prev == nullptr) {
        mHeadFreeList = block;
        block->mPrev = nullptr;
    } else {
        prev->mNext = block;
        block->mPrev = prev;
    }

    if (next == nullptr) {
        mTailFreeList = block;
        block->mNext = nullptr;
    } else {
        next->mPrev = block;
        block->mNext = next;
    }

    block->mMagic = 0;
}

void JKRExpHeap::removeFreeBlock(CMemBlock* block) {
    CMemBlock* prev = block->mPrev;
    CMemBlock* next = block->mNext;

    if (prev == nullptr) {
        mHeadFreeList = next;
    } else {
        prev->mNext = next;
    }

    if (next == nullptr) {
        mTailFreeList = prev;
    } else {
        next->mPrev = prev;
    }
}

void JKRExpHeap::removeUsedBlock(CMemBlock* block) {
    CMemBlock* prev = block->mPrev;
    CMemBlock* next = block->mNext;

    if (prev == nullptr) {
        mHeadUsedList = next;
    } else {
        prev->mNext = next;
    }

    if (next == nullptr) {
        mTailUsedList = prev;
    } else {
        next->mPrev = prev;
    }
}

void JKRExpHeap::recycleFreeBlock(CMemBlock* block) {
    CMemBlock* newBlock = block;
    int size = block->mSize;
    uintptr_t blockEnd = (uintptr_t)block + size;
    block->mMagic = 0;

    if ((block->mFlags & 0x7f) != 0) {
        newBlock = (CMemBlock*)((uintptr_t)block - (block->mFlags & 0x7f));
        size += (block->mFlags & 0x7f);
        blockEnd = (uintptr_t)newBlock + size;
        newBlock->mGroupId = 0;
        newBlock->mFlags = 0;
        newBlock->mSize = size;
    }

    if (!mHeadFreeList) {
        newBlock->initiate(nullptr, nullptr, size, 0, 0);
        mHeadFreeList = newBlock;
        mTailFreeList = newBlock;
        setFreeBlock(newBlock, nullptr, nullptr);
        return;
    }

    if ((uintptr_t)mHeadFreeList >= blockEnd) {
        newBlock->initiate(nullptr, nullptr, size, 0, 0);
        setFreeBlock(newBlock, nullptr, mHeadFreeList);
        joinTwoBlocks(newBlock);
        return;
    }

    if ((uintptr_t)mTailFreeList <= (uintptr_t)newBlock) {
        newBlock->initiate(nullptr, nullptr, size, 0, 0);
        setFreeBlock(newBlock, mTailFreeList, nullptr);
        joinTwoBlocks(newBlock->mPrev);
        return;
    }

    for (CMemBlock* freeBlock = mHeadFreeList; freeBlock; freeBlock = freeBlock->mNext) {
        if ((uintptr_t)freeBlock >= (uintptr_t)newBlock ||
            (uintptr_t)newBlock >= (uintptr_t)freeBlock->mNext) {
            continue;
        }

        newBlock->mNext = freeBlock->mNext;
        newBlock->mPrev = freeBlock;
        freeBlock->mNext = newBlock;
        newBlock->mNext->mPrev = newBlock;
        newBlock->mGroupId = 0;
        joinTwoBlocks(newBlock);
        joinTwoBlocks(freeBlock);
        return;
    }
}

void JKRExpHeap::joinTwoBlocks(CMemBlock* block) {
    uintptr_t endAddr = (uintptr_t)(block + 1) + block->mSize;
    CMemBlock* next = block->mNext;
    uintptr_t nextAddr = (uintptr_t)next - (next->mFlags & 0x7f);

    if (endAddr > nextAddr) {
        JUTWarningConsole_f(":::Heap may be broken. (block = %x)", block);
        OSReport(":::block = %x\n", block);
        OSReport(":::joinTwoBlocks [%x %x %x][%x %x %x]\n", block, block->mFlags, block->mSize,
                 block->mNext, block->mNext->mFlags, block->mNext->mSize);
        OSReport(":::: endAddr = %x\n", endAddr);
        OSReport(":::: nextAddr = %x\n", nextAddr);
        JKRHeap* heap = JKRGetCurrentHeap();
        heap->dump();
        OSPanic(__FILE__, 0, ":::: Bad Block\n");
    }

    if (endAddr == nextAddr) {
        block->mSize = next->mSize + sizeof(CMemBlock) + (next->mFlags & 0x7f) + block->mSize;
        CMemBlock* afterNext = next->mNext;
        setFreeBlock(block, block->mPrev, afterNext);
    }
}

bool JKRExpHeap::isEmpty() {
    return mHeadUsedList == nullptr;
}

void JKRExpHeap::adjustSize() {
    lock();

    if (mHeadUsedList != nullptr) {
        CMemBlock* block = mHeadUsedList;

        while (block->mNext != nullptr) {
            block = block->mNext;
        }

        u32 usedEnd = (u32)(uintptr_t)((u8*)block + sizeof(CMemBlock) + block->mSize);
        u32 newSize = usedEnd - (u32)(uintptr_t)mStart;

        if (newSize < mSize) {
            mSize = newSize;
            mEnd = mStart + mSize;
        }
    }

    unlock();
}

bool JKRExpHeap::check() {
    lock();
    int totalBytes = 0;
    bool ok = true;

    for (CMemBlock* block = mHeadUsedList; block; block = block->mNext) {
        if (block->mMagic != 'HM') {
            ok = false;
            JUTWarningConsole_f(":::addr %08x: bad heap signature. (%c%c)\n", block,
                                (block->mMagic >> 8) & 0xFF, block->mMagic & 0xFF);
        }

        if (block->mNext) {
            if (block->mNext->mMagic != 'HM') {
                ok = false;
                JUTWarningConsole_f(":::addr %08x: bad next pointer (%08x)\nabort\n", block,
                                    block->mNext);
                break;
            }

            if (block->mNext->mPrev != block) {
                ok = false;
                JUTWarningConsole_f(":::addr %08x: bad previous pointer (%08x)\n", block->mNext,
                                    block->mNext->mPrev);
            }
        } else {
            if (mTailUsedList != block) {
                ok = false;
                JUTWarningConsole_f(":::addr %08x: bad used list(REV) (%08x)\n", block,
                                    mTailUsedList);
            }
        }

        totalBytes += sizeof(CMemBlock) + block->mSize + (block->mFlags & 0x7f);
    }

    for (CMemBlock* block = mHeadFreeList; block; block = block->mNext) {
        totalBytes += block->mSize + sizeof(CMemBlock);

        if (block->mNext) {
            if (block->mNext->mPrev != block) {
                ok = false;
                JUTWarningConsole_f(":::addr %08x: bad previous pointer (%08x)\n", block->mNext,
                                    block->mNext->mPrev);
            }

            if ((uintptr_t)block + block->mSize + sizeof(CMemBlock) > (uintptr_t)block->mNext) {
                ok = false;
                JUTWarningConsole_f(":::addr %08x: bad block size (%08x)\n", block, block->mSize);
            }
        } else {
            if (mTailFreeList != block) {
                ok = false;
                JUTWarningConsole_f(":::addr %08x: bad free list(REV) (%08x)\n", block,
                                    mTailFreeList);
            }
        }
    }

    if (totalBytes != (int)mSize) {
        ok = false;
        JUTWarningConsole_f(":::bad total memory block size (%08X, %08X)\n", mSize, totalBytes);
    }

    if (!ok) {
        JUTWarningConsole_f(":::there is some error in this heap!\n");
    }

    unlock();
    return ok;
}

bool JKRExpHeap::dump() {
    lock();
    bool result = check();
    u32 usedBytes = 0;
    u32 usedCount = 0;
    u32 freeCount = 0;

    JUTReportConsole_f(" attr  address:   size    gid aln   prev_ptr next_ptr\n");
    JUTReportConsole_f("(Used Blocks)\n");

    if (!mHeadUsedList) {
        JUTReportConsole_f(" NONE\n");
    }

    for (CMemBlock* block = mHeadUsedList; block; block = block->mNext) {
        if (block->mMagic != 'HM') {
            JUTReportConsole_f("xxxxx %08x: --------  --- ---  (-------- --------)\nabort\n", block);
            break;
        }

        JUTReportConsole_f("%s %08x: %08x  %3d %3d  (%08x %08x)\n",
                           (block->mFlags & 0x80) ? " temp" : "alloc", block->getContent(),
                           block->mSize, block->mGroupId, block->mFlags & 0x7f, block->mPrev,
                           block->mNext);
        usedBytes += sizeof(CMemBlock) + block->mSize + (block->mFlags & 0x7f);
        usedCount++;
    }

    JUTReportConsole_f("(Free Blocks)\n");

    if (!mHeadFreeList) {
        JUTReportConsole_f(" NONE\n");
    }

    for (CMemBlock* block = mHeadFreeList; block; block = block->mNext) {
        JUTReportConsole_f("%s %08x: %08x  %3d %3d  (%08x %08x)\n", " free", block->getContent(),
                           block->mSize, block->mGroupId, block->mFlags & 0x7f, block->mPrev,
                           block->mNext);
        freeCount++;
    }

    float percent = ((float)usedBytes / (float)mSize) * 100.0f;
    JUTReportConsole_f("%d / %d bytes (%6.2f%%) used (U:%d F:%d)\n", usedBytes, mSize, percent,
                       usedCount, freeCount);
    unlock();
    return result;
}

bool JKRExpHeap::dump_sort() {
    return dump();
}

void JKRExpHeap::state_register(TState* p, u32 id) const {
    p->mId = id;
    JKRExpHeap* self = const_cast<JKRExpHeap*>(this);
    p->mUsedSize = mSize - self->do_getTotalFreeSize();

    u32 checkCode = 0;
    for (CMemBlock* block = mHeadUsedList; block; block = block->mNext) {
        checkCode += (u32)(uintptr_t)block * 3;
    }
    p->mCheckCode = checkCode;
}

bool JKRExpHeap::state_compare(const JKRHeap::TState& r1, const JKRHeap::TState& r2) const {
    bool result = true;
    if (r1.mCheckCode != r2.mCheckCode) {
        result = false;
    }
    if (r1.mUsedSize != r2.mUsedSize) {
        result = false;
    }
    return result;
}
