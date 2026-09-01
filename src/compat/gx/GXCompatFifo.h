#pragma once
// =============================================================================
// compat/gx — the simulated GX write-gather FIFO (M5.1).
//
// On the Wii, `GXWGFifo` is a hardware FIFO register (0xCC008000): the
// immediate-vertex writers (GXPosition3f32, GXColor4u8, ...) push raw values
// into it, and the vertex processor interprets the stream using the current
// vertex-descriptor (GX_VCD) + attribute formats (GX_VAT).
//
// On PC we keep the exact same model: the writers in GXVert.h still do
// `GXWGFifo._f32 = x` etc., but GXWGFifo is now a C++ object whose word
// members capture each write into the GX state machine in GXCompat.cpp. The
// byte/type distinction (_u8 vs _f32) is preserved so the state machine can
// interpret values exactly as the hardware would.
// =============================================================================

#include <cstdint>

// This header contains C++ types (GXFifoPipe/GXFifoWord) whose free helper
// functions must keep C++ linkage: they are implemented in compat/gx/
// GXCompat.cpp with C++ linkage. The vendored headers include it through
// revolution.h → os.h, which wraps everything in `extern "C"` — that would
// give fifoWrite* C linkage here and break the link. `extern "C++"` restores
// C++ linkage regardless of the enclosing block (it is a no-op when the
// header is already outside any extern "C").
#ifdef __cplusplus
extern "C++" {
#endif

namespace Platform { namespace CompatGx { namespace Detail {

// Pushes one raw value into the GX vertex stream.
void fifoWriteU8(std::uint8_t v);
void fifoWriteS8(std::int8_t v);
void fifoWriteU16(std::uint16_t v);
void fifoWriteS16(std::int16_t v);
void fifoWriteU32(std::uint32_t v);
void fifoWriteF32(float v);

}}} // namespace Platform::CompatGx::Detail

// A single word of the write-gather pipe: assigning to it captures the value
// with its exact type into the GX state machine.
struct GXFifoWord {
    GXFifoWord& operator=(std::uint8_t v) {
        Platform::CompatGx::Detail::fifoWriteU8(v);
        return *this;
    }
    GXFifoWord& operator=(std::int8_t v) {
        Platform::CompatGx::Detail::fifoWriteS8(v);
        return *this;
    }
    GXFifoWord& operator=(std::uint16_t v) {
        Platform::CompatGx::Detail::fifoWriteU16(v);
        return *this;
    }
    GXFifoWord& operator=(std::int16_t v) {
        Platform::CompatGx::Detail::fifoWriteS16(v);
        return *this;
    }
    GXFifoWord& operator=(std::uint32_t v) {
        Platform::CompatGx::Detail::fifoWriteU32(v);
        return *this;
    }
    GXFifoWord& operator=(float v) {
        Platform::CompatGx::Detail::fifoWriteF32(v);
        return *this;
    }
};

// The write-gather pipe: mirrors the PPCWGPipe union member names so the
// GXVert.h macros (`GXWGFifo._u8 = x`, `GXWGFifo._f32 = y`, ...) compile
// unchanged. Defined in GXCompat.cpp.
struct GXFifoPipe {
    GXFifoWord _u8;
    GXFifoWord _s8;
    GXFifoWord _u16;
    GXFifoWord _s16;
    GXFifoWord _u32;
    GXFifoWord _s32;
    GXFifoWord _u64;
    GXFifoWord _s64;
    GXFifoWord _f32;
    GXFifoWord _f64;
};

extern GXFifoPipe GXWGFifo;

#ifdef __cplusplus
}
#endif
