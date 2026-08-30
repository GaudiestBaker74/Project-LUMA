#include "tests/test_runner.h"

#include "platform/Endian/Endian.h"

#include <cstdint>
#include <cstring>

TEST_CASE(endian_swap32) {
    CHECK_EQ(Platform::Endian::swap32(0x11223344u), 0x44332211u);
    CHECK_EQ(Platform::Endian::swap32(0x00000000u), 0x00000000u);
    CHECK_EQ(Platform::Endian::swap32(0xFFFFFFFFu), 0xFFFFFFFFu);
    CHECK_EQ(Platform::Endian::swap16(0x1122u), 0x2211u);
    CHECK_EQ(Platform::Endian::swap64(0x1122334455667788ull), 0x8877665544332211ull);
}

TEST_CASE(endian_fromBE) {
    // On a little-endian host, fromBE32 must byteswap; the assertion below is
    // written to hold on either endianness by comparing with the byte layout.
    const uint32_t value = Platform::Endian::fromBE32(0x01020304u);
    uint8_t bytes[4];
    std::memcpy(bytes, &value, 4);
    CHECK_EQ(bytes[0], 0x01);
    CHECK_EQ(bytes[1], 0x02);
    CHECK_EQ(bytes[2], 0x03);
    CHECK_EQ(bytes[3], 0x04);
}

TEST_CASE(endian_stream_readers) {
    const uint8_t buffer[] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0};
    CHECK_EQ(Platform::Endian::readBE16(buffer), 0x1234u);
    CHECK_EQ(Platform::Endian::readBE32(buffer), 0x12345678u);
    CHECK_EQ(Platform::Endian::readBE64(buffer), 0x123456789ABCDEF0ull);
    CHECK_EQ(Platform::Endian::readBE16(buffer + 2), 0x5678u);
    CHECK_EQ(Platform::Endian::readBE32(buffer + 4), 0x9ABCDEF0u);
}

TEST_CASE(endian_stream_writers) {
    uint8_t out[14] = {0};
    Platform::Endian::writeBE16(out, 0x0102u);
    Platform::Endian::writeBE32(out + 2, 0x03040506u);
    Platform::Endian::writeBE64(out + 6, 0x0708090A0B0C0D0Eu);
    CHECK_EQ(out[0], 0x01);
    CHECK_EQ(out[1], 0x02);
    CHECK_EQ(out[2], 0x03);
    CHECK_EQ(out[3], 0x04);
    CHECK_EQ(out[4], 0x05);
    CHECK_EQ(out[5], 0x06);
    CHECK_EQ(out[6], 0x07);
    CHECK_EQ(out[7], 0x08);
    CHECK_EQ(out[8], 0x09);
    CHECK_EQ(out[13], 0x0E);
}

TEST_CASE(endian_float_bits) {
    CHECK_EQ(Platform::Endian::floatToBits(1.0f), 0x3F800000u);
    CHECK_EQ(Platform::Endian::bitsToFloat(0x3F800000u), 1.0f);
    CHECK_EQ(Platform::Endian::floatToBits(-2.5f), 0xC0200000u);
    CHECK_NEAR(Platform::Endian::readBEF32("\x3F\x80\x00\x00"), 1.0f, 1e-6f);

    uint8_t out[4] = {0};
    Platform::Endian::writeBEF32(out, 0.5f);
    CHECK_EQ(out[0], 0x3F);
    CHECK_EQ(out[1], 0x00);
    CHECK_EQ(out[2], 0x00);
    CHECK_EQ(out[3], 0x00);
}

TEST_CASE(endian_roundtrip) {
    const uint32_t values[] = {0, 1, 0x80000000u, 0xDEADBEEFu, 0xFFFFFFFFu};
    for (uint32_t v : values) {
        CHECK_EQ(Platform::Endian::fromBE32(Platform::Endian::toBE32(v)), v);
    }
}
