#include <revolution/gd/GDBase.h>

// PC_PORT PATCH (patched copy of third_party/petari/src/RVL_SDK/gd/GDBase.c;
// the vendored original is not modified). The only change is in
// GDFlushCurrToMem: the original calls DCFlushRange (a PPC data-cache flush),
// which does not exist on host toolchains and is a no-op on PC (cache
// coherence is handled by the hardware). Everything else is byte-for-byte
// identical to upstream — the display-list recording state machine the J3D
// renderer builds lists with (GDInitGDLObj/GDSetCurrent/GDPadCurr32).

GDLObj* __GDCurrentDL = NULL;
static GDOverflowCallback overflowcb = NULL;

void GDInitGDLObj(GDLObj* pDL, void* pStart, u32 length) {
    pDL->start = pStart;
    pDL->ptr = (u8*)pStart;
    pDL->top = (u8*)pStart + length;
    pDL->length = length;
}

void GDFlushCurrToMem() {
    // PC_PORT: DCFlushRange removed (PPC data-cache flush — no-op on PC).
    (void)__GDCurrentDL;
}

void GDPadCurr32() {
    u32 n = ((u32)__GDCurrentDL->ptr & 31);
    if (n) {
        for (; n < 32; n++) {
            __GDWrite(0);
        }
    }
}

void GDOverflowed() {
    if (overflowcb) {
        (*overflowcb)();
    }
}
