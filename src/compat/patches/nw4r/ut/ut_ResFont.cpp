// =============================================================================
// PC_PORT PATCH of the vendored nw4r/ut/ut_ResFont.cpp (see
// src/compat/patches/README.md).
//
// Changes vs. upstream:
//   1. The console SetResource/Rebuild reinterpret the brfnt file image
//      directly as FontInformation/FontTextureGlyph/FontWidth/FontCodeMap
//      structs and resolve the embedded 32-bit file offsets to pointers IN
//      PLACE (`ResolveOffset` casts the pointer to s32). That only works on a
//      32-bit big-endian target: the file is big-endian AND the on-disk
//      pointer fields are 4 bytes while the host structs embed 8-byte
//      pointers (different sizeof and field offsets — the structs cannot
//      overlay the file at all).
//      The patched version parses the BE blocks with explicit offset reads
//      and builds a native struct tree in one allocation (the model of
//      compat/jsystem/RarcHost and compat/gx/TplHost). Sheet image payloads
//      are NOT copied — the native FontTextureGlyph points into the original
//      blob (raw texels need no byte swapping).
//   2. The native tree is cached per source blob and never freed (a game
//      session holds a handful of fonts for its whole lifetime; the original
//      blobs live inside mounted archives that outlive any single ResFont).
//      mResource keeps pointing at the ORIGINAL buffer, so IsManaging()/
//      RemoveResource() semantics for game code are unchanged.
//   3. Rebuild() (the public in-place fixup) cannot exist on the host; it is
//      not called anymore. The declaration in ResFont.h stays unused.
//
// On-disk brfnt (big-endian):
//   BinaryFileHeader @0: 'RFNT', u16 byteOrder (0xFEFF), u16 version
//     (0x0102/0x0104), u32 fileSize, u16 headerSize, u16 dataBlocks
//   Blocks (kind[4] + u32 size), walked from headerSize:
//     'FINF': u8 fontType, s8 linefeed, u16 alterCharIndex,
//             CharWidths defaultWidth (3 x u8), u8 encoding,
//             u32 glyphOff, u32 widthOff, u32 mapOff,   // 0 = absent
//             u8 height, u8 width, u8 ascent, u8 pad
//     'TGLP': u8 cellWidth, u8 cellHeight, s8 baselinePos, u8 maxCharWidth,
//             u32 sheetSize, u16 sheetNum, u16 sheetFormat, u16 sheetRow,
//             u16 sheetLine, u16 sheetWidth, u16 sheetHeight, u32 sheetOff
//     'CWDH': u16 indexBegin, u16 indexEnd, u32 nextOff, CharWidths[]
//     'CMAP': u16 ccodeBegin, u16 ccodeEnd, u16 mappingMethod, u16 reserved,
//             u32 nextOff, u16 mapInfo[]
//     'GLGR': ignored (grouping hint)
//   All nextOff/*Off values are relative to the file start.
// =============================================================================
#include "nw4r/ut/ResFont.h"
#include "nw4r/ut/binaryFileFormat.h"

#include "platform/Log/Log.h"

#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

using namespace nw4r::ut;

u16 readBE16(const u8* p) {
    return static_cast<u16>((static_cast<u32>(p[0]) << 8) | p[1]);
}

u32 readBE32(const u8* p) {
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
           (static_cast<u32>(p[2]) << 8) | static_cast<u32>(p[3]);
}

// A converted font: the native FontInformation plus the single allocation
// backing it (glyph/width/map structs).
struct HostFont {
    FontInformation* info = nullptr;  // points into storage
    std::vector<u8> storage;
};

std::unordered_map<void*, HostFont*>& fontCache() {
    static std::unordered_map<void*, HostFont*> cache;
    return cache;
}

std::mutex& fontCacheMutex() {
    static std::mutex mutex;
    return mutex;
}

bool isBeBrfnt(const u8* p) {
    return p != nullptr && p[0] == 'R' && p[1] == 'F' && p[2] == 'N' && p[3] == 'T' &&
           readBE16(p + 4) == 0xFEFF;
}

struct DiskBlock {
    u32 kind = 0;         // BE-read as chars: 'FINF' etc. (memcmp on name)
    char name[5] = {0};
    u32 offset = 0;       // data offset (after the 8-byte block header)
    u32 size = 0;         // total block size (header included)
};

HostFont* convert(const u8* src, u32 fileSizeHint = 0) {
    const u16 version = readBE16(src + 6);
    const u32 fileSize = readBE32(src + 8);
    const u32 headerSize = readBE16(src + 12);
    const u32 dataBlocks = readBE16(src + 14);

    if (version != 0x0102 && version != 0x0104) {
        PL_LOG_WARN("compat.font", "brfnt: unsupported version 0x%x", version);
        return nullptr;
    }
    (void)fileSizeHint;
    if (fileSize < headerSize || dataBlocks == 0 || dataBlocks > 1024) {
        PL_LOG_WARN("compat.font", "brfnt: implausible header (fileSize %u, blocks %u)",
                    fileSize, dataBlocks);
        return nullptr;
    }

    // Pass 1: walk the block table.
    std::vector<DiskBlock> blocks;
    blocks.reserve(dataBlocks);
    u32 pos = headerSize;
    const s32 finfDiskOffset = 0;  // FINF data offset resolved below
    (void)finfDiskOffset;
    for (u32 i = 0; i < dataBlocks; ++i) {
        if (pos + 8 > fileSize) {
            PL_LOG_WARN("compat.font", "brfnt: block %u header past EOF", i);
            return nullptr;
        }
        DiskBlock b;
        std::memcpy(b.name, src + pos, 4);
        b.size = readBE32(src + pos + 4);
        b.offset = pos + 8;
        if (b.size < 8 || pos + b.size > fileSize) {
            PL_LOG_WARN("compat.font", "brfnt: block '%s' size %u past EOF", b.name, b.size);
            return nullptr;
        }
        blocks.push_back(b);
        pos += b.size;
    }

    const DiskBlock* finf = nullptr;
    const DiskBlock* tglp = nullptr;
    std::vector<const DiskBlock*> cwdhBlocks;
    std::vector<const DiskBlock*> cmapBlocks;
    for (const DiskBlock& b : blocks) {
        if (std::memcmp(b.name, "FINF", 4) == 0) {
            finf = &b;
        } else if (std::memcmp(b.name, "TGLP", 4) == 0 || std::memcmp(b.name, "TGLF", 4) == 0) {
            tglp = &b;
        } else if (std::memcmp(b.name, "CWDH", 4) == 0) {
            cwdhBlocks.push_back(&b);
        } else if (std::memcmp(b.name, "CMAP", 4) == 0) {
            cmapBlocks.push_back(&b);
        } else if (std::memcmp(b.name, "GLGR", 4) != 0) {
            PL_LOG_WARN("compat.font", "brfnt: unknown block '%.4s' ignored", b.name);
        }
    }
    if (finf == nullptr || tglp == nullptr || finf->size - 8 < 24 || tglp->size - 8 < 24) {
        PL_LOG_WARN("compat.font", "brfnt: missing FINF/TGLP block");
        return nullptr;
    }
    if (cwdhBlocks.empty() || cmapBlocks.empty()) {
        PL_LOG_WARN("compat.font", "brfnt: font without CWDH/CMAP blocks is not usable");
        return nullptr;
    }

    const u8* finfData = src + finf->offset;
    const u32 glyphDiskOff = readBE32(finfData + 8);
    const u32 widthDiskOff = readBE32(finfData + 12);
    const u32 mapDiskOff = readBE32(finfData + 16);

    // Pass 2: size the native storage.
    //   FontInformation + FontTextureGlyph
    // + per CWDH: FontWidth + widthTable entries
    // + per CMAP: FontCodeMap + mapInfo payload (u16 elements).
    size_t storageBytes = sizeof(FontInformation) + sizeof(FontTextureGlyph);
    storageBytes = (storageBytes + 7) & ~static_cast<size_t>(7);

    struct WidthPlan {
        const DiskBlock* block;
        u32 diskDataOff;  // file offset of the FontWidth data
        u16 count;
        size_t nativeOff;
    };
    struct MapPlan {
        const DiskBlock* block;
        u32 diskDataOff;
        u16 infoCount;  // u16 elements after the fixed part
        size_t nativeOff;
    };
    std::vector<WidthPlan> widthPlans;
    std::vector<MapPlan> mapPlans;

    for (const DiskBlock* b : cwdhBlocks) {
        const u8* d = src + b->offset;
        const u16 indexBegin = readBE16(d + 0);
        const u16 indexEnd = readBE16(d + 2);
        const u32 payload = b->size - 8;  // block size minus the block header
        if (payload < 8) {
            PL_LOG_WARN("compat.font", "brfnt: short CWDH block");
            return nullptr;
        }
        u32 count = (indexEnd >= indexBegin) ? (u32(indexEnd - indexBegin) + 1) : 0;
        // Trust the block size when it disagrees with the index range.
        const u32 maxCount = (payload - 8) / sizeof(CharWidths);
        if (count > maxCount) {
            count = maxCount;
        }
        WidthPlan plan;
        plan.block = b;
        plan.diskDataOff = b->offset;
        plan.count = static_cast<u16>(count);
        plan.nativeOff = storageBytes;
        storageBytes += sizeof(FontWidth) + size_t(count) * sizeof(CharWidths);
        storageBytes = (storageBytes + 7) & ~static_cast<size_t>(7);
        widthPlans.push_back(plan);
    }

    for (const DiskBlock* b : cmapBlocks) {
        const u32 payload = b->size - 8;
        if (payload < 12) {
            PL_LOG_WARN("compat.font", "brfnt: short CMAP block");
            return nullptr;
        }
        const u16 infoCount = static_cast<u16>((payload - 12) / 2);
        MapPlan plan;
        plan.block = b;
        plan.diskDataOff = b->offset;
        plan.infoCount = infoCount;
        plan.nativeOff = storageBytes;
        storageBytes += sizeof(FontCodeMap) + size_t(infoCount) * sizeof(u16);
        storageBytes = (storageBytes + 7) & ~static_cast<size_t>(7);
        mapPlans.push_back(plan);
    }

    // Pass 3: build the native structs.
    HostFont* host = new HostFont();
    host->storage.assign(storageBytes, 0);
    u8* base = host->storage.data();

    auto* info = reinterpret_cast<FontInformation*>(base);
    info->fontType = finfData[0];
    info->linefeed = static_cast<s8>(finfData[1]);
    info->alterCharIndex = readBE16(finfData + 2);
    info->defaultWidth.left = static_cast<s8>(finfData[4]);
    info->defaultWidth.glyphWidth = finfData[5];
    info->defaultWidth.charWidth = static_cast<s8>(finfData[6]);
    info->encoding = finfData[7];
    info->height = finfData[20];
    info->width = finfData[21];
    info->ascent = finfData[22];

    auto* glyph = reinterpret_cast<FontTextureGlyph*>(base + sizeof(FontInformation));
    {
        const u8* d = src + tglp->offset;
        glyph->cellWidth = d[0];
        glyph->cellHeight = d[1];
        glyph->baselinePos = static_cast<s8>(d[2]);
        glyph->maxCharWidth = d[3];
        glyph->sheetSize = readBE32(d + 4);
        glyph->sheetNum = readBE16(d + 8);
        glyph->sheetFormat = readBE16(d + 10);
        glyph->sheetRow = readBE16(d + 12);
        glyph->sheetLine = readBE16(d + 14);
        glyph->sheetWidth = readBE16(d + 16);
        glyph->sheetHeight = readBE16(d + 18);
        const u32 sheetOff = readBE32(d + 20);
        // Sheet payload stays in the source blob (raw texels, no swapping).
        glyph->sheetImage = (sheetOff != 0 && sheetOff < fileSize)
                                ? const_cast<u8*>(src) + sheetOff
                                : nullptr;
        (void)glyphDiskOff;
    }
    info->pGlyph = glyph;

    // Widths: native copies + pNext chain following the disk offsets.
    for (size_t i = 0; i < widthPlans.size(); ++i) {
        const WidthPlan& plan = widthPlans[i];
        const u8* d = src + plan.diskDataOff;
        auto* w = reinterpret_cast<FontWidth*>(base + plan.nativeOff);
        w->indexBegin = readBE16(d + 0);
        w->indexEnd = readBE16(d + 2);
        const u32 nextOff = readBE32(d + 4);
        w->pNext = nullptr;
        if (nextOff != 0) {
            for (const WidthPlan& other : widthPlans) {
                if (other.diskDataOff == nextOff) {
                    w->pNext = reinterpret_cast<FontWidth*>(base + other.nativeOff);
                    break;
                }
            }
        }
        // widthTable: 3-byte POD entries, copied verbatim.
        std::memcpy(reinterpret_cast<u8*>(w->widthTable), d + 8,
                    size_t(plan.count) * sizeof(CharWidths));
    }
    info->pWidth = nullptr;
    if (widthDiskOff != 0) {
        for (const WidthPlan& plan : widthPlans) {
            if (plan.diskDataOff == widthDiskOff) {
                info->pWidth = reinterpret_cast<FontWidth*>(base + plan.nativeOff);
                break;
            }
        }
    }
    if (info->pWidth == nullptr && !widthPlans.empty()) {
        info->pWidth = reinterpret_cast<FontWidth*>(base + widthPlans[0].nativeOff);
    }

    // Code maps: same treatment; mapInfo is u16 elements (swap each).
    for (const MapPlan& plan : mapPlans) {
        const u8* d = src + plan.diskDataOff;
        auto* m = reinterpret_cast<FontCodeMap*>(base + plan.nativeOff);
        m->ccodeBegin = readBE16(d + 0);
        m->ccodeEnd = readBE16(d + 2);
        m->mappingMethod = readBE16(d + 4);
        m->reserved = readBE16(d + 6);
        const u32 nextOff = readBE32(d + 8);
        m->pNext = nullptr;
        if (nextOff != 0) {
            for (const MapPlan& other : mapPlans) {
                if (other.diskDataOff == nextOff) {
                    m->pNext = reinterpret_cast<FontCodeMap*>(base + other.nativeOff);
                    break;
                }
            }
        }
        for (u16 i = 0; i < plan.infoCount; ++i) {
            m->mapInfo[i] = readBE16(d + 12 + u32(i) * 2);
        }
    }
    info->pMap = nullptr;
    if (mapDiskOff != 0) {
        for (const MapPlan& plan : mapPlans) {
            if (plan.diskDataOff == mapDiskOff) {
                info->pMap = reinterpret_cast<FontCodeMap*>(base + plan.nativeOff);
                break;
            }
        }
    }
    if (info->pMap == nullptr && !mapPlans.empty()) {
        info->pMap = reinterpret_cast<FontCodeMap*>(base + mapPlans[0].nativeOff);
    }

    host->info = info;
    return host;
}

} // namespace

namespace nw4r {
    namespace ut {

        ResFont::ResFont() {
        }

        ResFont::~ResFont() {
        }

        bool ResFont::SetResource(void* brfnt) {
            FontInformation* pFontInfo = NULL;

            if (!IsManaging(NULL)) {
                return false;
            }
            if (brfnt == NULL) {
                return false;
            }

            // PC_PORT (M9.5.2): the file image is big-endian with 32-bit
            // offset fields; the native structs embed host pointers and
            // cannot overlay it (see the file banner). Convert once per
            // source blob (cached); mResource keeps pointing at the original
            // buffer so IsManaging/RemoveResource behave as upstream.
            if (isBeBrfnt(static_cast<u8*>(brfnt))) {
                std::lock_guard<std::mutex> lock(fontCacheMutex());
                auto& cache = fontCache();
                const auto it = cache.find(brfnt);
                HostFont* host;
                if (it != cache.end() && isBeBrfnt(static_cast<u8*>(brfnt))) {
                    host = it->second;
                } else {
                    host = convert(static_cast<u8*>(brfnt));
                    if (host == nullptr) {
                        return false;
                    }
                    cache[brfnt] = host;  // process-lifetime (see banner)
                    PL_LOG_INFO("compat.font", "brfnt converted: %dx%d, encoding %d",
                                (int)host->info->width, (int)host->info->height,
                                (int)host->info->encoding);
                }
                pFontInfo = host->info;
            } else if (std::memcmp(brfnt, "RFNU", 4) == 0) {
                // Host-built font (native structs, pointers resolved).
                BinaryBlockHeader* blockHeader =
                    reinterpret_cast<BinaryBlockHeader*>(static_cast<u8*>(brfnt) +
                                                         reinterpret_cast<BinaryFileHeader*>(brfnt)->headerSize);
                const BinaryFileHeader* fileHeader = static_cast<BinaryFileHeader*>(brfnt);
                for (int nBlocks = 0; nBlocks < fileHeader->dataBlocks; ++nBlocks) {
                    if (std::memcmp(&blockHeader->kind, "FINF", 4) == 0) {
                        pFontInfo = reinterpret_cast<FontInformation*>(
                            reinterpret_cast<u8*>(blockHeader) + sizeof(*blockHeader));
                        break;
                    }
                    blockHeader = reinterpret_cast<BinaryBlockHeader*>(
                        reinterpret_cast<u8*>(blockHeader) + blockHeader->size);
                }
            } else {
                PL_LOG_WARN("compat.font", "SetResource: not a brfnt (%p)", brfnt);
                return false;
            }

            if (pFontInfo == NULL) {
                return false;
            }

            SetResourceBuffer(brfnt, pFontInfo);
            InitReaderFunc(GetEncoding());

            return true;
        }

        void ResFont::RemoveResource() {
            // PC_PORT: the converted native tree stays in the host cache
            // (shared by every ResFont attached to the same blob, and the
            // original buffer is archive-owned). Only detach, as upstream.
            RemoveResourceBuffer();
        }

    };  // namespace ut
};  // namespace nw4r
