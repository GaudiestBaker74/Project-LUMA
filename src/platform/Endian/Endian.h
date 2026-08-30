#pragma once
// =============================================================================
// Platform::Endian — big-endian helpers.
//
// All game data files are big-endian (they were authored for the PowerPC Wii).
// The game's parsers are mostly byte-oriented and survive an LE host, but any
// code that must read/write BE values through this module stays explicit.
// =============================================================================

#include <bit>
#include <cstdint>
#include <cstring>

namespace Platform::Endian {

constexpr bool hostIsLittleEndian() {
    return std::endian::native == std::endian::little;
}

inline uint16_t swap16(uint16_t v) {
    return static_cast<uint16_t>((v << 8) | (v >> 8));
}

inline uint32_t swap32(uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}

inline uint64_t swap64(uint64_t v) {
    return (static_cast<uint64_t>(swap32(static_cast<uint32_t>(v))) << 32) |
           swap32(static_cast<uint32_t>(v >> 32));
}

// Host value -> big-endian value (value unchanged on BE hosts).
inline uint16_t toBE16(uint16_t v) { return hostIsLittleEndian() ? swap16(v) : v; }
inline uint32_t toBE32(uint32_t v) { return hostIsLittleEndian() ? swap32(v) : v; }
inline uint64_t toBE64(uint64_t v) { return hostIsLittleEndian() ? swap64(v) : v; }

// Big-endian value -> host value.
inline uint16_t fromBE16(uint16_t v) { return toBE16(v); }
inline uint32_t fromBE32(uint32_t v) { return toBE32(v); }
inline uint64_t fromBE64(uint64_t v) { return toBE64(v); }

// --- Stream readers/writers (byte-oriented, endianness-independent) ---------

inline uint16_t readBE16(const void* p) {
    const auto* b = static_cast<const uint8_t*>(p);
    return static_cast<uint16_t>((b[0] << 8) | b[1]);
}

inline uint32_t readBE32(const void* p) {
    const auto* b = static_cast<const uint8_t*>(p);
    return (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
           (static_cast<uint32_t>(b[2]) << 8) | static_cast<uint32_t>(b[3]);
}

inline uint64_t readBE64(const void* p) {
    const auto* b = static_cast<const uint8_t*>(p);
    uint64_t hi = readBE32(b);
    uint64_t lo = readBE32(b + 4);
    return (hi << 32) | lo;
}

inline void writeBE16(void* p, uint16_t v) {
    auto* b = static_cast<uint8_t*>(p);
    b[0] = static_cast<uint8_t>(v >> 8);
    b[1] = static_cast<uint8_t>(v);
}

inline void writeBE32(void* p, uint32_t v) {
    auto* b = static_cast<uint8_t*>(p);
    b[0] = static_cast<uint8_t>(v >> 24);
    b[1] = static_cast<uint8_t>(v >> 16);
    b[2] = static_cast<uint8_t>(v >> 8);
    b[3] = static_cast<uint8_t>(v);
}

inline void writeBE64(void* p, uint64_t v) {
    writeBE32(p, static_cast<uint32_t>(v >> 32));
    writeBE32(static_cast<uint8_t*>(p) + 4, static_cast<uint32_t>(v));
}

// --- Float <-> u32 bit casts (endianness-safe) ------------------------------

inline float bitsToFloat(uint32_t bits) {
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

inline uint32_t floatToBits(float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

// Reads a big-endian f32 from a byte stream.
inline float readBEF32(const void* p) {
    return bitsToFloat(readBE32(p));
}

inline void writeBEF32(void* p, float f) {
    writeBE32(p, floatToBits(f));
}

} // namespace Platform::Endian
