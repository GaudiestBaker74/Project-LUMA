// =============================================================================
// PC_PORT PATCH of the vendored JSystem/JKernel/JKRDecomp.cpp (see
// patches/README.md).
//
// M9.5: Yaz0/Yay0 decompression, needed by JKRArchive mounting (every
// LayoutData/ObjectData .arc.szs in the game goes through here).
//
// Changes vs. upstream:
//
// 1. **Endianness (the critical one).** `decodeSZS` read the header with
//    `*(u32*)pSrc` and `*(u32*)(pSrc + 4)`. On the PPC32 console the host is
//    big-endian and the Yaz0 stream is big-endian, so that plain load happens
//    to be correct. On x86-64/ARM64 the same load byteswaps the value:
//    the decompressed size becomes garbage and the loop bound is wrong, so
//    every .arc.szs mount silently produces corrupt data or runs off the end
//    of the destination buffer. All multi-byte reads now go through explicit
//    big-endian helpers (same thing `decodeSZP` already did with its
//    READU32_BE macro).
//
// 2. **64-bit pointers.** `decodeSZS` computed its end pointer as
//    `((s32)pDst + size) - a4`, truncating the destination address to 32
//    bits. On a 64-bit host that is a wrong end pointer and the loop never
//    terminates where it should. Computed with `uintptr_t` now.
//
// 3. **Synchronous decode.** `orderSync` posted a command to the decompress
//    thread (JKRDecomp::run) and blocked on a message queue. That thread
//    exists only to overlap DVD streaming with decode on console hardware;
//    the host reads archives from local disk and `orderSync` is by contract
//    blocking, so it now decodes in place and returns. `create`,
//    `prepareCommand`, `sendCommand` and `sync` are kept as no-op-compatible
//    stubs so any caller still links. `run()` is unreachable but kept for
//    signature parity.
//
// 4. `decodeSZP` (Yay0) keeps the upstream algorithm; only its end-pointer
//    arithmetic is made 64-bit safe. Yay0 is not used by SMG's archives but
//    the path is exercised by the RARC "sub-archive" branch.
// =============================================================================
#include "JSystem/JKernel/JKRDecomp.hpp"
#include "JSystem/JKernel/JKRHeap.hpp"
#include "revolution.h"

#include <cstdint>

namespace {
    // Big-endian readers for the Yaz0/Yay0 stream. The console was BE, so the
    // decompilation's plain `*(u32*)p` loads were correct there and are wrong
    // on every host we build for.
    inline u32 readBE32(const u8* p) {
        return (static_cast< u32 >(p[0]) << 24) | (static_cast< u32 >(p[1]) << 16) | (static_cast< u32 >(p[2]) << 8) |
               static_cast< u32 >(p[3]);
    }

    inline u16 readBE16(const u8* p) {
        return static_cast< u16 >((static_cast< u32 >(p[0]) << 8) | static_cast< u32 >(p[1]));
    }
};  // namespace

JKRDecompCommand::JKRDecompCommand() {
    OSInitMessageQueue(&mMessageQueue, &mMessage, 1);
    mThis = this;
    _14 = 0;
    _1C = nullptr;
    _20 = 0;
}

JKRDecomp::JKRDecomp(s32 a1) : JKRThread(0x4000, 0x10, a1) {
    // PC_PORT: the console resumes the decompress thread here. The host
    // decodes synchronously (see orderSync), so there is nothing to resume.
}

JKRDecomp::~JKRDecomp() {
}

void* JKRDecomp::run() {
    // PC_PORT: unreachable on the host — no decompress thread is started.
    return nullptr;
}

JKRDecomp* JKRDecomp::create(s32 a1) {
    (void)a1;
    // PC_PORT: no worker thread needed; orderSync decodes inline.
    return nullptr;
}

JKRDecompCommand* JKRDecomp::prepareCommand(unsigned char* pSrc, unsigned char* pDst, u32 compressedSize,
                                           u32 decompressedSize, void (*a5)(u32)) {
    JKRDecompCommand* command = new (JKRHeap::sGameHeap, -4) JKRDecompCommand();

    command->mSrc = pSrc;
    command->mDst = pDst;
    command->mCompressedSize = static_cast< u32 >(compressedSize);
    command->mDecompressedSize = static_cast< u32 >(decompressedSize);
    command->_14 = a5;

    return command;
}

void JKRDecomp::sendCommand(JKRDecompCommand* pCommand) {
    // PC_PORT: decode immediately instead of queueing for the worker thread.
    // Callers of sendCommand follow it with sync(), which is a no-op here.
    if (pCommand == nullptr) {
        return;
    }

    decode(pCommand->mSrc, pCommand->mDst, pCommand->mCompressedSize, pCommand->mDecompressedSize);

    if (pCommand->_14 != nullptr) {
        pCommand->_14(static_cast< u32 >(reinterpret_cast< uintptr_t >(pCommand)));
    }
}

bool JKRDecomp::sync(JKRDecompCommand* pCommand, int noBlock) {
    (void)pCommand;
    (void)noBlock;
    // PC_PORT: sendCommand/orderSync already finished the work.
    return true;
}

bool JKRDecomp::orderSync(unsigned char* pSrc, unsigned char* pDst, u32 compressedSize,
                         u32 decompressedSize) {
    // PC_PORT: synchronous by contract — decode in place.
    decode(pSrc, pDst, compressedSize, decompressedSize);
    return true;
}

void JKRDecomp::decode(unsigned char* pSrc, unsigned char* pDst, u32 compressedSize,
                      u32 decompressedSize) {
    EJKRCompression compression = checkCompressed(pSrc);

    if (compression == JKR_COMPRESSION_SZP) {
        decodeSZP(pSrc, pDst, static_cast< u32 >(compressedSize), static_cast< u32 >(decompressedSize));
    } else if (compression == JKR_COMPRESSION_SZS) {
        decodeSZS(pSrc, pDst, static_cast< u32 >(compressedSize), static_cast< u32 >(decompressedSize));
    }
}

void JKRDecomp::decodeSZP(u8* src, u8* dst, u32 srcLength, u32 dstLength) {
    int srcChunkOffset;
    int count;
    int dstOffset;
    u32 length = srcLength;
    int linkInfo;
    int offset;
    int i;

    int decodedSize = static_cast< int >(readBE32(src + 4));
    int linkTableOffset = static_cast< int >(readBE32(src + 8));
    int srcDataOffset = static_cast< int >(readBE32(src + 12));

    dstOffset = 0;
    u32 counter = 0;
    srcChunkOffset = 16;

    u32 chunkBits = 0;
    if (srcLength == 0)
        return;
    if (dstLength > static_cast< u32 >(decodedSize))
        return;

    do {
        if (counter == 0) {
            chunkBits = readBE32(src + srcChunkOffset);
            srcChunkOffset += sizeof(u32);
            counter = sizeof(u32) * 8;
        }

        if (chunkBits & 0x80000000) {
            if (dstLength == 0) {
                dst[dstOffset] = src[srcDataOffset];
                length--;
                if (length == 0)
                    return;
            } else {
                dstLength--;
            }
            dstOffset++;
            srcDataOffset++;
        } else {
            linkInfo = readBE16(src + linkTableOffset);
            linkTableOffset += sizeof(u16);

            offset = dstOffset - (linkInfo & 0xFFF);
            count = (linkInfo >> 12);
            if (count == 0) {
                count = static_cast< u32 >(src[srcDataOffset++]) + 0x12;
            } else
                count += 2;

            if (count > decodedSize - dstOffset)
                count = decodedSize - dstOffset;

            for (i = 0; i < count; i++, dstOffset++, offset++) {
                if (dstLength == 0) {
                    dst[dstOffset] = dst[offset - 1];
                    length--;
                    if (length == 0)
                        return;
                } else
                    dstLength--;
            }
        }

        chunkBits <<= 1;
        counter--;
    } while (dstOffset < decodedSize);
}

// PC_PORT: rewritten for the host. Upstream read the header with plain
// `*(u32*)` loads (correct on big-endian PPC, byteswapped on x86-64/ARM64) and
// computed the end pointer through an `(s32)pDst` truncation. Both are fixed
// here; the LZ77 algorithm itself is unchanged.
void JKRDecomp::decodeSZS(u8* pSrc, u8* pDst, u32 compressedSize, u32 a4) {
    const u32 decompressedSize = readBE32(pSrc + 4);

    // Upstream: decompSize = ((s32)pDst + *(u32*)(pSrc+4)) - a4, i.e. the
    // address one past the last byte to write when `a4` bytes are skipped.
    u8* const endPtr = pDst + (static_cast< uintptr_t >(decompressedSize) - a4);

    u8 byte1, byte2;
    s32 validBitCount = 0;
    u32 curBlock = 0;

    if (compressedSize == 0) {
        return;
    }

    if (a4 > decompressedSize) {
        return;
    }

    pSrc += 0x10;

    do {
        if (validBitCount == 0) {
            curBlock = *pSrc;
            validBitCount = 8;
            pSrc++;
        }

        if ((curBlock & 0x80) != 0) {
            if (a4 == 0) {
                compressedSize--;
                *pDst++ = *pSrc;

                if (compressedSize == 0) {
                    return;
                }
            } else {
                a4--;
            }

            pSrc++;
        } else {
            byte1 = *pSrc++;
            byte2 = *pSrc++;

            // Back-reference: the 12-bit field stores (distance-1), so the
            // first source byte is pDst - distance == copySrc - 1.
            u8* copySrc = pDst;
            copySrc -= ((byte1 & 0xF) << 8) | byte2;

            u32 numBytes = byte1 >> 4;

            if (numBytes == 0) {
                numBytes = *pSrc++ + 0x12;
            } else {
                numBytes += 2;
            }

            do {
                if (a4 == 0) {
                    compressedSize--;
                    *pDst = *(copySrc - 1);
                    pDst++;

                    if (compressedSize == 0) {
                        return;
                    }
                } else {
                    a4--;
                }

                copySrc++;
            } while (--numBytes != 0);
        }

        curBlock <<= 1;
        validBitCount--;
    } while (pDst != endPtr);
}

EJKRCompression JKRDecomp::checkCompressed(unsigned char* pSrc) {
    if (pSrc[0] == 'Y' && pSrc[1] == 'a' && pSrc[3] == '0') {
        if (pSrc[2] == 'y') {
            return JKR_COMPRESSION_SZP;
        }

        if (pSrc[2] == 'z') {
            return JKR_COMPRESSION_SZS;
        }
    }

    if (pSrc[0] == 'A' && pSrc[1] == 'S' && pSrc[2] == 'R') {
        return JKR_COMPRESSION_ASR;
    }

    return JKR_COMPRESSION_NONE;
}
