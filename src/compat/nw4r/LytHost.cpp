// =============================================================================
// compat/nw4r — brlyt big-endian -> host conversion. See LytHost.h.
//
// Section map (offsets relative to the start of each data block; every block
// starts with kind[4] + u32 size). Field lists follow the res:: structs in
// libs/nw4r/include/nw4r/lyt/resources.h and the parsing order of the lyt
// constructors (lyt_layout/lyt_material/lyt_pane/lyt_picture/lyt_textBox/
// lyt_window/lyt_group):
//
//   file header: byteOrder u16 @4 (0xFEFF BE mark, doubles as the swapped
//     flag), version u16 @6, fileSize u32 @8, headerSize u16 @12,
//     dataBlocks u16 @14
//   lyt1: layoutSize 2 x f32 @12
//   txl1/fnl1: count u16 @8; count x {u32 nameStrOffset @+0} entries @12
//   mat1: count u16 @8; count x u32 material offsets @12 (relative to the
//     mat1 block); each material: tevCols 12 x s16 @20, resNum u32 @60,
//     then the variable tail (order fixed by the Material ctor):
//       TexMap[texMapNum]    4B  (u16 texIdx swapped)
//       TexSRT[texSRTNum]    20B (5 x f32)
//       TexCoordGen[n]       4B  (bytes)
//       ChanCtrl?            4B  (bytes)
//       matCol?              4B  (bytes)
//       TevSwapMode?         4 x 1B
//       indTexSRT[n]         20B (5 x f32)
//       IndirectStage[n]     4B  (bytes)
//       TevStage[n]          16B (bytes)
//       AlphaCompare?        4B  (bytes)
//       BlendMode?           4B  (bytes)
//   pan1/bnd1: block size u32 @4; 10 x f32 @36 (translate/rotate/scale/size)
//   pic1: pane fields; materialIdx u16 @92; texCoords @96: n x 32B (8 x f32)
//   txt1: pane fields; textBufBytes/textStrBytes/materialIdx/fontIdx u16
//     @76..82; textStrOffset u32 @88; fontSize/charSpace/lineSpace f32
//     @100..112; the string itself (UTF-16BE) is left alone (the patched
//     lyt_textBox.cpp reads it as BE u16).
//   wnd1: pane fields; inflation 4 x f32 @76; contentOffset u32 @96;
//     frameOffsetTableOffset u32 @100; frame offset table @104: frameNum x
//     u32 (relative to the wnd1 block), each WindowFrame: materialIdx u16;
//     content @contentOffset: WindowContent (materialIdx u16 @+16) followed
//     by texCoordNum x 32B texcoords.
//   grp1: paneNum u16 @24; pane names (16B each) untouched
//   grs1/gre1/pas1/pae1: header only
// =============================================================================
#include "compat/nw4r/LytHost.h"

#include "platform/Log/Log.h"

#include <cstring>
#include <vector>

namespace {

u16 loadU16(const u8* p) {
    u16 v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

u32 loadU32(const u8* p) {
    u32 v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

void swap16At(u8* p) {
    const u16 v = loadU16(p);
    const u16 s = static_cast<u16>((v >> 8) | (v << 8));
    std::memcpy(p, &s, sizeof(s));
}

void swap32At(u8* p) {
    const u32 v = loadU32(p);
    const u32 s = ((v >> 24) & 0x000000FFu) | ((v >> 8) & 0x0000FF00u) |
                  ((v << 8) & 0x00FF0000u) | ((v << 24) & 0xFF000000u);
    std::memcpy(p, &s, sizeof(s));
}

// Swaps n consecutive u32-sized words (f32 payloads included: only the byte
// order matters, not the interpretation).
void swap32Range(u8* p, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        swap32At(p + i * 4);
    }
}

bool kindIs(const u8* block, const char* kind) {
    return std::memcmp(block, kind, 4) == 0;
}

struct Swapper {
    u8* base = nullptr;
    u32 fileSize = 0;

    bool inBounds(u32 off, u32 need) const {
        return need <= fileSize && off <= fileSize - need;
    }

    // --- sections -----------------------------------------------------------

    bool swapMaterial(u32 off) {
        // resNum u32 @60 must be swapped before the tail counts are read.
        if (!inBounds(off, 64)) {
            return false;
        }
        swap32Range(base + off + 20, 6);  // tevCols: 3 x GXColorS10 (4 s16)
        swap32At(base + off + 60);        // resNum bits

        const u32 bits = loadU32(base + off + 60);
        const u32 texMapNum = bits & 0xF;
        const u32 texSRTNum = (bits >> 4) & 0xF;
        const u32 texCoordGenNum = (bits >> 8) & 0xF;
        const bool tevSwap = ((bits >> 12) & 0x1) != 0;
        const u32 indTexSRTNum = (bits >> 13) & 0x3;
        const u32 indTexStageNum = (bits >> 15) & 0x7;
        const u32 tevStageNum = (bits >> 18) & 0x1F;
        const bool alphaCompare = ((bits >> 23) & 0x1) != 0;
        const bool blendMode = ((bits >> 24) & 0x1) != 0;
        const u32 chanCtrlNum = (bits >> 25) & 0x1;
        const u32 matColNum = (bits >> 27) & 0x1;

        u32 pos = off + 64;
        // TexMap[]: u16 texIdx per 4-byte entry.
        for (u32 i = 0; i < texMapNum; ++i) {
            if (!inBounds(pos, 2)) {
                return false;
            }
            swap16At(base + pos);
            pos += 4;
        }
        // TexSRT[]: 5 x f32.
        if (texSRTNum > 0) {
            if (!inBounds(pos, texSRTNum * 20)) {
                return false;
            }
            swap32Range(base + pos, texSRTNum * 5);
            pos += texSRTNum * 20;
        }
        // TexCoordGen[]: 4 bytes each, no multi-byte fields.
        pos += texCoordGenNum * 4;
        // ChanCtrl / matCol / TevSwapMode x4: byte fields only.
        pos += chanCtrlNum * 4;
        pos += matColNum * 4;
        pos += tevSwap ? 4 : 0;
        // Indirect TexSRT[]: 5 x f32.
        if (indTexSRTNum > 0) {
            if (!inBounds(pos, indTexSRTNum * 20)) {
                return false;
            }
            swap32Range(base + pos, indTexSRTNum * 5);
            pos += indTexSRTNum * 20;
        }
        // IndirectStage[] (4B), TevStage[] (16B), AlphaCompare (4B),
        // BlendMode (4B): byte fields only — nothing to swap.
        pos += indTexStageNum * 4;
        pos += tevStageNum * 16;
        pos += alphaCompare ? 4 : 0;
        pos += blendMode ? 4 : 0;
        if (!inBounds(pos, 0)) {
            return false;
        }
        return true;
    }

    void swapPaneFields(u32 off) {
        // Transform/size f32s @36..75. (The block size u32 @4 is already
        // swapped by swapBlock before dispatch — do NOT swap it twice.)
        swap32Range(base + off + 36, 10);
    }

    void swapTexCoords(u32 off, u32 num) {
        // num x VEC2[4] = num x 8 x f32.
        if (num > 0 && inBounds(off, num * 32)) {
            swap32Range(base + off, num * 8);
        }
    }

    bool swapBlock(u32 off) {
        if (!inBounds(off, 8)) {
            return false;
        }
        u8* blk = base + off;
        swap32At(blk + 4);  // block size
        const u32 size = loadU32(blk + 4);
        if (size < 8 || !inBounds(off, size)) {
            PL_LOG_WARN("compat.lyt", "brlyt: block '%.4s' size %u out of bounds", blk, size);
            return false;
        }

        if (kindIs(blk, "lyt1")) {
            if (!inBounds(off, 20)) {
                return false;
            }
            swap32Range(blk + 12, 2);  // layoutSize
        } else if (kindIs(blk, "txl1") || kindIs(blk, "fnl1")) {
            if (!inBounds(off, 12)) {
                return false;
            }
            swap16At(blk + 8);
            const u32 n = loadU16(blk + 8);
            for (u32 i = 0; i < n; ++i) {  // entries: u32 nameStrOffset + u8s
                if (!inBounds(off + 12 + i * 8, 4)) {
                    return false;
                }
                swap32At(blk + 12 + i * 8);
            }
        } else if (kindIs(blk, "mat1")) {
            if (!inBounds(off, 12)) {
                return false;
            }
            swap16At(blk + 8);
            const u32 n = loadU16(blk + 8);
            for (u32 i = 0; i < n; ++i) {
                const u32 tblOff = off + 12 + i * 4;
                if (!inBounds(tblOff, 4)) {
                    return false;
                }
                swap32At(base + tblOff);
                // Material offsets are relative to the mat1 block start
                // (ConvertOffsToPtr(pMaterialList, tbl[i]) in the ctors).
                const u32 matOff = off + loadU32(base + tblOff);
                if (!swapMaterial(matOff)) {
                    PL_LOG_WARN("compat.lyt", "brlyt: material %u out of bounds", i);
                    return false;
                }
            }
        } else if (kindIs(blk, "pan1") || kindIs(blk, "bnd1")) {
            if (!inBounds(off, 76)) {
                return false;
            }
            swapPaneFields(off);
        } else if (kindIs(blk, "pic1")) {
            if (!inBounds(off, 96)) {
                return false;
            }
            swapPaneFields(off);
            swap16At(blk + 92);  // materialIdx
            const u32 texCoordNum = blk[94];
            swapTexCoords(off + 96, texCoordNum);
        } else if (kindIs(blk, "txt1")) {
            if (!inBounds(off, 116)) {
                return false;
            }
            swapPaneFields(off);
            swap16At(blk + 76);  // textBufBytes
            swap16At(blk + 78);  // textStrBytes
            swap16At(blk + 80);  // materialIdx
            swap16At(blk + 82);  // fontIdx
            swap32At(blk + 88);  // textStrOffset
            swap32Range(blk + 100, 4);  // fontSize + charSpace + lineSpace
            // The UTF-16BE string at textStrOffset is read big-endian by the
            // patched lyt_textBox.cpp — deliberately not swapped.
        } else if (kindIs(blk, "wnd1")) {
            if (!inBounds(off, 104)) {
                return false;
            }
            swapPaneFields(off);
            swap32Range(blk + 76, 4);  // inflation
            const u32 frameNum = blk[92];
            swap32At(blk + 96);        // contentOffset
            swap32At(blk + 100);       // frameOffsetTableOffset
            // Frame offset table: frameNum x u32, each pointing at a
            // WindowFrame {materialIdx u16, textureFlip u8, pad u8}.
            u32 tbl = off + 104;
            for (u32 i = 0; i < frameNum; ++i) {
                if (!inBounds(tbl, 4)) {
                    return false;
                }
                swap32At(base + tbl);
                // WindowFrame offsets are relative to the wnd1 block start.
                const u32 frmOff = off + loadU32(base + tbl);
                if (!inBounds(frmOff, 4)) {
                    return false;
                }
                swap16At(base + frmOff);  // materialIdx
                tbl += 4;
            }
            // contentOffset is also relative to the wnd1 block start.
            const u32 contentRel = loadU32(blk + 96);
            if (contentRel != 0) {
                const u32 contentOff = off + contentRel;
                if (!inBounds(contentOff, 20)) {
                    return false;
                }
                swap16At(base + contentOff + 16);  // materialIdx
                const u32 texCoordNum = base[contentOff + 18];
                swapTexCoords(contentOff + 20, texCoordNum);
            }
        } else if (kindIs(blk, "grp1")) {
            if (!inBounds(off, 28)) {
                return false;
            }
            swap16At(blk + 24);  // paneNum
            // Pane name list (16-byte names) follows — bytes, nothing to do.
        } else if (kindIs(blk, "grs1") || kindIs(blk, "gre1") || kindIs(blk, "pas1") ||
                   kindIs(blk, "pae1")) {
            // Nesting markers — header only.
        } else if (kindIs(blk, "usd1")) {
            PL_LOG_WARN("compat.lyt", "brlyt: usd1 (extended user data) not converted");
        } else {
            PL_LOG_WARN("compat.lyt", "brlyt: unknown block '%.4s' left as-is", blk);
        }
        return true;
    }
};

// =============================================================================
// brlan (RLAN) swapper (M9.5.3b).
//
// Section map (offsets relative to the start of each block; every block
// starts with kind[4] + u32 size):
//
//   file header: same BinaryFileHeader as brlyt (magic 'RLAN')
//   pai1 (res::AnimationBlock): frameSize u16 @8, loop u8 @10, fileNum u16
//     @12, animContNum u16 @14, animContOffsetsOffset u32 @16; then
//     u32[fileNum] texture-name offsets @20 (array-relative strings), and at
//     animContOffsetsOffset u32[animContNum] content offsets (block-relative).
//     Content ("animation"): name[0x14], tagNum u8 @0x14, targetKind u8
//     @0x15 (0=pane, 1=material), u16 @0x16, u32[tagNum] tag offsets @0x18
//     (content-relative).
//     Tag: kind[4] ('RLPA'/'RLTS'/'RLVI'/'RLVC'/'RLMC'/'RLTP'), entryNum u8
//     @4, u32[entryNum] entry offsets @8 (tag-relative).
//     Entry: index u8 @0 (texture slot), target u8 @1 (property id), keyType
//     u8 @2 (1=step, 2=hermite), keyNum u16 @4, u16 @6, keysOffset u32 @8
//     (entry-relative, always 0x0C); keys: hermite = keyNum x 3 f32
//     (frame, value, slope); step = keyNum x {f32 frame, u16 value, u16 pad}.
//   tag1 (res::AnimationTagBlock): tagOrder u16 @8, groupNum u16 @10,
//     nameOffset u32 @12, groupsOffset u32 @16, startFrame s16 @20,
//     endFrame s16 @22, flag u8 @24. Group refs are strings/bytes.
//   shr1 (res::AnimationShareBlock): animShareInfoOffset u32 @8, shareNum
//     u16 @12. Share infos are strings/bytes.
//   pat1 (MKW texture-pattern block): not used by SMG — left as-is.
// =============================================================================
struct BrlanSwapper {
    u8* base = nullptr;
    u32 fileSize = 0;

    bool inBounds(u32 off, u32 need) const {
        return need <= fileSize && off <= fileSize - need;
    }

    bool swapEntry(u32 eoff) {
        if (!inBounds(eoff, 0x0C)) {
            return false;
        }
        swap16At(base + eoff + 4);  // keyNum
        swap16At(base + eoff + 6);  // unknown u16
        swap32At(base + eoff + 8);  // keysOffset

        const u32 keyType = base[eoff + 2];
        const u32 keyNum = loadU16(base + eoff + 4);
        const u32 keysOff = loadU32(base + eoff + 8);

        if (keyNum == 0) {
            return true;
        }

        const u32 keySize = (keyType == 2) ? 12u : (keyType == 1) ? 8u : 0u;
        if (keySize == 0) {
            PL_LOG_WARN("compat.lyt", "brlan: unknown key type %u at entry 0x%x (keys left as-is)",
                        keyType, eoff);
            return true;
        }
        if (!inBounds(eoff + keysOff, keyNum * keySize)) {
            return false;
        }

        u8* keys = base + eoff + keysOff;
        if (keyType == 2) {
            swap32Range(keys, keyNum * 3);  // frame, value, slope (all f32)
        } else {
            for (u32 i = 0; i < keyNum; ++i) {
                swap32At(keys + i * 8);      // frame f32
                swap16At(keys + i * 8 + 4);  // value u16
            }
        }
        return true;
    }

    bool swapTag(u32 toff) {
        if (!inBounds(toff, 8)) {
            return false;
        }
        const u32 entryNum = base[toff + 4];
        if (entryNum == 0) {
            return true;
        }
        if (!inBounds(toff + 8, entryNum * 4)) {
            return false;
        }

        std::vector<u32> entryOffsets(entryNum);
        for (u32 i = 0; i < entryNum; ++i) {
            swap32At(base + toff + 8 + i * 4);
            entryOffsets[i] = loadU32(base + toff + 8 + i * 4);
        }
        for (u32 i = 0; i < entryNum; ++i) {
            if (!swapEntry(toff + entryOffsets[i])) {
                return false;
            }
        }
        return true;
    }

    bool swapContent(u32 coff) {
        if (!inBounds(coff, 0x18)) {
            return false;
        }
        swap16At(base + coff + 0x16);  // unknown u16

        const u32 tagNum = base[coff + 0x14];
        if (tagNum == 0) {
            return true;
        }
        if (!inBounds(coff + 0x18, tagNum * 4)) {
            return false;
        }

        std::vector<u32> tagOffsets(tagNum);
        for (u32 i = 0; i < tagNum; ++i) {
            swap32At(base + coff + 0x18 + i * 4);
            tagOffsets[i] = loadU32(base + coff + 0x18 + i * 4);
        }
        for (u32 i = 0; i < tagNum; ++i) {
            if (!swapTag(coff + tagOffsets[i])) {
                return false;
            }
        }
        return true;
    }

    bool swapPai1(u32 off) {
        if (!inBounds(off, 0x14)) {
            return false;
        }
        swap16At(base + off + 8);    // frameSize
        swap16At(base + off + 0xC);  // fileNum
        swap16At(base + off + 0xE);  // animContNum
        swap32At(base + off + 0x10); // animContOffsetsOffset

        const u32 fileNum = loadU16(base + off + 0xC);
        const u32 animContNum = loadU16(base + off + 0xE);
        const u32 contOffsOff = loadU32(base + off + 0x10);

        // Texture-file-name offsets (strings live relative to the array).
        if (fileNum > 0) {
            if (!inBounds(off + 0x14, fileNum * 4)) {
                return false;
            }
            swap32Range(base + off + 0x14, fileNum);
        }

        if (animContNum == 0) {
            return true;
        }
        if (!inBounds(off + contOffsOff, animContNum * 4)) {
            return false;
        }

        std::vector<u32> contentOffsets(animContNum);
        for (u32 i = 0; i < animContNum; ++i) {
            swap32At(base + off + contOffsOff + i * 4);
            contentOffsets[i] = loadU32(base + off + contOffsOff + i * 4);
        }
        for (u32 i = 0; i < animContNum; ++i) {
            if (!swapContent(off + contentOffsets[i])) {
                return false;
            }
        }
        return true;
    }

    bool swapBlock(u32 off) {
        if (!inBounds(off, 8)) {
            return false;
        }
        u8* const blk = base + off;
        swap32At(blk + 4);  // block size

        if (kindIs(blk, "pai1")) {
            return swapPai1(off);
        } else if (kindIs(blk, "tag1")) {
            if (!inBounds(off, 0x1C)) {
                return false;
            }
            swap16At(blk + 8);    // tagOrder
            swap16At(blk + 0xA);  // groupNum
            swap32At(blk + 0xC);  // nameOffset
            swap32At(blk + 0x10); // groupsOffset
            swap16At(blk + 0x14); // startFrame
            swap16At(blk + 0x16); // endFrame
            // flag u8 @0x18; group refs are strings/bytes.
        } else if (kindIs(blk, "shr1")) {
            if (!inBounds(off, 0x10)) {
                return false;
            }
            swap32At(blk + 8);   // animShareInfoOffset
            swap16At(blk + 0xC); // shareNum
        } else if (kindIs(blk, "pat1")) {
            PL_LOG_WARN("compat.lyt", "brlan: pat1 (texture patterns) not converted");
        } else {
            PL_LOG_WARN("compat.lyt", "brlan: unknown block '%.4s' left as-is", blk);
        }
        return true;
    }
};

} // namespace

namespace Platform::CompatLyt {

bool isBrlyt(const void* data, u32 dataSize) {
    return data != nullptr && dataSize >= 16 &&
           std::memcmp(data, "RLYT", 4) == 0;
}

bool convertBrlyt(void* data, u32 dataSize) {
    u8* p = static_cast<u8*>(data);
    if (p == nullptr || (dataSize != 0 && dataSize < 16)) {
        return false;
    }
    if (std::memcmp(p, "RLYT", 4) != 0) {
        return false;
    }

    // byteOrder: BE files carry FE FF on disk (a little-endian read gives
    // 0xFFFE); after conversion the read gives 0xFEFF. That doubles as the
    // idempotency marker.
    const u16 orderRaw = loadU16(p + 4);
    if (orderRaw == 0xFEFF) {
        return true;  // already host-native
    }
    if (orderRaw != 0xFFFE) {
        PL_LOG_WARN("compat.lyt", "brlyt: bad byte-order mark 0x%04x", orderRaw);
        return false;
    }

    swap16At(p + 4);   // byteOrder
    swap16At(p + 6);   // version
    swap32At(p + 8);   // fileSize
    swap16At(p + 12);  // headerSize
    swap16At(p + 14);  // dataBlocks

    Swapper swapper;
    swapper.base = p;
    swapper.fileSize = loadU32(p + 8);
    if (dataSize != 0 && swapper.fileSize > dataSize) {
        PL_LOG_WARN("compat.lyt", "brlyt: header fileSize %u > buffer %u", swapper.fileSize,
                    dataSize);
        return false;
    }
    const u32 headerSize = loadU16(p + 12);
    const u32 dataBlocks = loadU16(p + 14);
    if (headerSize < 16 || !swapper.inBounds(headerSize, 0)) {
        PL_LOG_WARN("compat.lyt", "brlyt: bad headerSize %u", headerSize);
        return false;
    }

    u32 pos = headerSize;
    for (u32 i = 0; i < dataBlocks; ++i) {
        if (!swapper.swapBlock(pos)) {
            PL_LOG_WARN("compat.lyt", "brlyt: conversion aborted at block %u (offset 0x%x)", i,
                        pos);
            return false;
        }
        // swapBlock already swapped this block's size word.
        pos += loadU32(p + pos + 4);
    }
    return true;
}

bool isBrlan(const void* data, u32 dataSize) {
    return data != nullptr && dataSize >= 16 &&
           std::memcmp(data, "RLAN", 4) == 0;
}

bool convertBrlan(void* data, u32 dataSize) {
    u8* p = static_cast<u8*>(data);
    if (p == nullptr || (dataSize != 0 && dataSize < 16)) {
        return false;
    }
    if (std::memcmp(p, "RLAN", 4) != 0) {
        return false;
    }

    // Same idempotency marker as convertBrlyt: BE files carry FE FF on disk
    // (a little-endian read gives 0xFFFE); after conversion the read gives
    // 0xFEFF.
    const u16 orderRaw = loadU16(p + 4);
    if (orderRaw == 0xFEFF) {
        return true;  // already host-native
    }
    if (orderRaw != 0xFFFE) {
        PL_LOG_WARN("compat.lyt", "brlan: bad byte-order mark 0x%04x", orderRaw);
        return false;
    }

    swap16At(p + 4);   // byteOrder
    swap16At(p + 6);   // version
    swap32At(p + 8);   // fileSize
    swap16At(p + 12);  // headerSize
    swap16At(p + 14);  // dataBlocks

    BrlanSwapper swapper;
    swapper.base = p;
    swapper.fileSize = loadU32(p + 8);
    if (dataSize != 0 && swapper.fileSize > dataSize) {
        PL_LOG_WARN("compat.lyt", "brlan: header fileSize %u > buffer %u", swapper.fileSize,
                    dataSize);
        return false;
    }
    const u32 headerSize = loadU16(p + 12);
    const u32 dataBlocks = loadU16(p + 14);
    if (headerSize < 16 || !swapper.inBounds(headerSize, 0)) {
        PL_LOG_WARN("compat.lyt", "brlan: bad headerSize %u", headerSize);
        return false;
    }

    u32 pos = headerSize;
    for (u32 i = 0; i < dataBlocks; ++i) {
        if (!swapper.swapBlock(pos)) {
            PL_LOG_WARN("compat.lyt", "brlan: conversion aborted at block %u (offset 0x%x)", i,
                        pos);
            return false;
        }
        pos += loadU32(p + pos + 4);
    }
    return true;
}

} // namespace Platform::CompatLyt
