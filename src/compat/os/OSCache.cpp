// compat/os — data-cache range operations (OSCache.h), no-ops on PC.
//
// The Wii game uses DCFlushRange/DCInvalidateRange/DCStoreRange/DCZeroRange
// to keep the L1 data cache coherent with the DSP/AI DMA. On PC the game and
// the emulated DSP share one memory space with no DMA caches, so these are
// no-ops. Same reasoning as the GD patch (patches/RVL_SDK/gd/GDBase.c).
#include <cstdint>

extern "C" {

void DCInvalidateRange(void*, uint32_t) {}
void DCFlushRange(void*, uint32_t) {}
void DCStoreRange(void*, uint32_t) {}
void DCFlushRangeNoSync(void*, uint32_t) {}
void DCStoreRangeNoSync(void*, uint32_t) {}
void DCZeroRange(void*, uint32_t) {}
void DCEnable(void) {}

} // extern "C"
