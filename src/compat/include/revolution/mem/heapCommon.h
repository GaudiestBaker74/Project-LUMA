#ifndef HEAPCOMMON_H
#define HEAPCOMMON_H

#include <cstdint>
#include <cstring>

#ifdef __cplusplus
extern "C" {
#endif

#include <revolution/mem.h>  // PC_PORT: upstream had the short <mem.h> (Metrowerks include path); full path avoids a self-include
#include <revolution/mem/list.h>
#include <revolution/os.h>

typedef struct MEMiHeapHead MEMiHeapHead;

struct MEMiHeapHead {
    u32 signature;
    MEMLink link;
    MEMList childList;
    void* heapStart;
    void* heapEnd;
    OSMutex mutex;

    union {
        u32 val;

        struct {
            u32 reserved : 24;
            u32 optFlag : 8;
        } fields;
    } attribute;
};

typedef MEMiHeapHead* MEMHeapHandle;

// PC_PORT: upstream types UIntPtr as u32, which truncates pointers on 64-bit
// hosts (same defect as types.h ROUND_UP_PTR). Keep the name (game code may
// rely on it) but widen to uintptr_t.
typedef uintptr_t UIntPtr;

static inline UIntPtr GetUIntPtr(const void* ptr) {
    return (UIntPtr)(ptr);
}

static inline u32 GetOffsetFromPtr(const void* start, const void* end) {
    return GetUIntPtr(end) - GetUIntPtr(start);
}

static inline void* SubU32ToPtr(void* ptr, u32 val) {
    return (void*)(GetUIntPtr(ptr) - val);
}

static inline void* AddU32ToPtr(void* ptr, u32 val) {
    return (void*)(GetUIntPtr(ptr) + val);
}

static inline void SetOptForHeap(MEMiHeapHead* pHeapHd, u16 optFlag) {
    pHeapHd->attribute.fields.optFlag = (u8)optFlag;
}

static inline u16 GetOptForHeap(const MEMiHeapHead* pHeapHd) {
    return (u16)pHeapHd->attribute.fields.optFlag;
}

static inline void FillAllocMemory(MEMiHeapHead* pHeapHd, void* address, u32 size) {
    if (GetOptForHeap(pHeapHd) & 1) {
        (void)memset(address, 0, size);
    }
}

static inline int ComparePtr(const void* a, const void* b) {
    const u8* wa = (const u8*)a;
    const u8* wb = (const u8*)b;

    return wa - wb;
}

// PC_PORT: cast through uintptr_t so the macro does not truncate 64-bit
// pointers (upstream mask is u32-wide).
#define RoundUp(value, alignment) (((uintptr_t)(value) + ((uintptr_t)(alignment) - 1)) & ~((uintptr_t)(alignment) - 1))

#define RoundUpPtr(ptr, alignment) ((void*)RoundUp(GetUIntPtr(ptr), (alignment)))

#define RoundDown(value, alignment) ((uintptr_t)(value) & ~((uintptr_t)(alignment) - 1))

#define RoundDownPtr(ptr, alignment) ((void*)RoundDown(GetUIntPtr(ptr), (alignment)))

static inline void LockHeap(MEMiHeapHead* pHeapHd) {
    if (GetOptForHeap(pHeapHd) & 4) {
        OSLockMutex(&pHeapHd->mutex);
    }
}

static inline void UnlockHeap(MEMiHeapHead* pHeapHd) {
    if (GetOptForHeap(pHeapHd) & 4) {
        OSUnlockMutex(&pHeapHd->mutex);
    }
}

void MEMiInitHeapHead(MEMiHeapHead* pHeapHd, u32 signature, void* heapStart, void* heapEnd, u16 optFlag);

void MEMiFinalizeHeap(MEMiHeapHead* pHeapHd);

void MEMiDumpHeapHead(MEMiHeapHead* pHeapHd);

#ifdef __cplusplus
}
#endif

#endif  // HEAPCOMMON_H
