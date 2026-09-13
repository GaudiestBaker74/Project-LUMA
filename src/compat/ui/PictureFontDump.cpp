// =============================================================================
// compat/ui — PictureFontDump (see PictureFontDump.h).
//
// The brfnt layout below is the one documented at the top of the vendor patch
// that loads these fonts (patches/nw4r/ut/ut_ResFont.cpp), so the dump reads
// exactly what the port's ResFont reads:
//
//   BinaryFileHeader @0: 'RFNT', u16 byteOrder, u16 version, u32 fileSize,
//                        u16 headerSize, u16 dataBlocks
//   Blocks (kind[4] + u32 size, walked from headerSize):
//     'FINF' u8 fontType, s8 linefeed, u16 alterCharIndex, u8 defaultWidth[3],
//            u8 encoding, u32 glyphOff, u32 widthOff, u32 mapOff,
//            u8 height, u8 width, u8 ascent, u8 pad
//     'TGLP' u8 cellWidth, u8 cellHeight, s8 baselinePos, u8 maxCharWidth,
//            u32 sheetSize, u16 sheetNum, u16 sheetFormat, u16 sheetRow,
//            u16 sheetLine, u16 sheetWidth, u16 sheetHeight, u32 sheetOff
//     'CMAP' u16 ccodeBegin, u16 ccodeEnd, u16 mappingMethod, u16 reserved,
//            u32 nextOff, u16 mapInfo[]
//   All *Off values are relative to the file start; the sheet image itself is
//   GX-tiled, so it goes through the port's own btiDecodeToRgba8.
// =============================================================================

#include "compat/ui/PictureFontDump.h"

#include "compat/gx/Bti.h"
#include "platform/Log/Log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace compat::ui {

namespace {

u16 be16(const u8* p) {
    return static_cast<u16>((static_cast<u32>(p[0]) << 8) | p[1]);
}

u32 be32(const u8* p) {
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
           (static_cast<u32>(p[2]) << 8) | static_cast<u32>(p[3]);
}

bool isKind(u32 kind, const char* name) {
    return kind == ((static_cast<u32>(name[0]) << 24) | (static_cast<u32>(name[1]) << 16) |
                    (static_cast<u32>(name[2]) << 8) | static_cast<u32>(name[3]));
}

const char* formatName(u16 format) {
    switch (format) {
    case 0: return "I4";
    case 1: return "I8";
    case 2: return "IA4";
    case 3: return "IA8";
    case 4: return "RGB565";
    case 5: return "RGB5A3";
    case 6: return "RGBA8";
    case 0xE: return "CMPR";
    default: return "?";
    }
}

/// Row-major RGBA8 -> binary PPM (P6), nearest-neighbour `scale`, alpha
/// flattened on mid gray so both dark and light glyphs are readable.
bool writePpm(const std::string& path, const u8* rgba, int w, int h, int scale,
              const u8* backdrop = nullptr, int bw = 0, int bh = 0) {
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (fp == nullptr) {
        return false;
    }
    if (backdrop != nullptr && bw > 0 && bh > 0 && w == bw * scale && h == bh * scale) {
        // The canvas already is the image to write (contact sheet).
        std::fprintf(fp, "P6\n%d %d\n255\n", w, h);
        std::fwrite(backdrop, 1, static_cast<size_t>(w) * h * 3, fp);
        std::fclose(fp);
        return true;
    }
    std::fprintf(fp, "P6\n%d %d\n255\n", w * scale, h * scale);
    std::vector<u8> row(static_cast<size_t>(w) * scale * 3);
    for (int y = 0; y < h * scale; ++y) {
        const u8* src = rgba + static_cast<size_t>(y / scale) * w * 4;
        for (int x = 0; x < w * scale; ++x) {
            const u8* px = src + static_cast<size_t>(x / scale) * 4;
            const int a = px[3];
            row[static_cast<size_t>(x) * 3 + 0] = static_cast<u8>((px[0] * a + 128 * (255 - a)) / 255);
            row[static_cast<size_t>(x) * 3 + 1] = static_cast<u8>((px[1] * a + 128 * (255 - a)) / 255);
            row[static_cast<size_t>(x) * 3 + 2] = static_cast<u8>((px[2] * a + 128 * (255 - a)) / 255);
        }
        std::fwrite(row.data(), 1, row.size(), fp);
    }
    std::fclose(fp);
    return true;
}

struct Sheet {
    u8 cellW = 0;
    u8 cellH = 0;
    u16 format = 0;
    u16 gridW = 0;   // glyphs per row
    u16 gridH = 0;   // rows
    u16 width = 0;   // sheet image size
    u16 height = 0;
    std::vector<u8> rgba;
    bool decoded = false;
};

/// One code -> cell mapping, in file order (the contact sheet uses this order).
struct CodeEntry {
    u16 code = 0;
    int glyph = -1;
    int sheet = -1;
    int col = 0;
    int row = 0;
};

}  // namespace

bool dumpPictureFontIfRequested(const void* brfntData) {
    const char* dir = std::getenv("LUMA_PICFONT_DUMP");
    if (dir == nullptr || dir[0] == '\0') {
        return false;
    }

    std::string outDir(dir);
    if (outDir.empty() || outDir.back() != '/') {
        outDir += '/';
    }

    PL_LOG_INFO("compat.font", "LUMA_PICFONT_DUMP=%s: dumping the picture font", dir);

    if (brfntData == nullptr) {
        PL_LOG_WARN("compat.font",
                    "picture font dump: no /PictureFont.brfnt mounted (is /LayoutData/Font.arc "
                    "in the assets tree?)");
        return false;
    }

    const u8* f = static_cast<const u8*>(brfntData);
    if (std::memcmp(f, "RFNT", 4) != 0) {
        PL_LOG_WARN("compat.font", "picture font dump: not a brfnt (no RFNT magic)");
        return false;
    }

    const u32 fileSize = be32(f + 8);
    const u16 headerSize = be16(f + 12);
    const u16 dataBlocks = be16(f + 14);

    if (fileSize < 32 || fileSize > (64u << 20)) {
        PL_LOG_WARN("compat.font", "picture font dump: implausible fileSize %u", fileSize);
        return false;
    }

    PL_LOG_INFO("compat.font", "picture font: RFNT, %u bytes, header %u, %u data block(s)",
                fileSize, headerSize, dataBlocks);

    std::vector<Sheet> sheets;
    std::vector<CodeEntry> entries;

    for (u32 off = headerSize; off + 8 <= fileSize;) {
        const u8* b = f + off;
        const u32 kind = be32(b);
        const u32 blockSize = be32(b + 4);

        if (isKind(kind, "FINF") && blockSize >= 32) {
            PL_LOG_INFO("compat.font",
                        "  FINF: type=%u encoding=%u height=%u width=%u ascent=%u "
                        "alterChar=%u glyphOff=%u widthOff=%u mapOff=%u",
                        b[8], b[15], b[28], b[29], b[30], be16(b + 10), be32(b + 16),
                        be32(b + 20), be32(b + 24));
        } else if (isKind(kind, "TGLP") && blockSize >= 32) {
            Sheet s;
            s.cellW = b[8];
            s.cellH = b[9];
            const u32 sheetSize = be32(b + 12);
            s.format = be16(b + 18);
            s.gridW = be16(b + 20);
            s.gridH = be16(b + 22);
            s.width = be16(b + 24);
            s.height = be16(b + 26);
            const u32 sheetOff = be32(b + 28);

            PL_LOG_INFO("compat.font",
                        "  TGLP sheet %u: %ux%u %s, grid %ux%u, cell %ux%u, size %u, off %u",
                        static_cast<unsigned>(sheets.size()), s.width, s.height,
                        formatName(s.format), s.gridW, s.gridH, s.cellW, s.cellH, sheetSize,
                        sheetOff);

            if (sheetOff != 0 && sheetOff + sheetSize <= fileSize && s.width > 0 && s.height > 0) {
                const size_t need =
                    Platform::CompatGx::btiImageSize(s.width, s.height, static_cast<u8>(s.format));
                if (need > 0 && sheetSize >= need) {
                    s.rgba.resize(static_cast<size_t>(s.width) * s.height * 4);
                    if (Platform::CompatGx::btiDecodeToRgba8(
                            f + sheetOff, sheetSize, s.width, s.height,
                            static_cast<u8>(s.format), nullptr, 0, 0, s.rgba.data())) {
                        s.decoded = true;
                        const std::string path = outDir + "picfont_sheet" +
                                                 std::to_string(sheets.size()) + ".ppm";
                        if (writePpm(path, s.rgba.data(), s.width, s.height, 1)) {
                            PL_LOG_INFO("compat.font", "  -> %s", path.c_str());
                        }
                    } else {
                        PL_LOG_WARN("compat.font", "  sheet %u: btiDecodeToRgba8 failed",
                                    static_cast<unsigned>(sheets.size()));
                    }
                } else {
                    PL_LOG_WARN("compat.font",
                                "  sheet %u: %u bytes but the decoder needs %u for %s",
                                static_cast<unsigned>(sheets.size()), sheetSize,
                                static_cast<unsigned>(need), formatName(s.format));
                }
            }
            sheets.push_back(s);
        } else if (isKind(kind, "CMAP") && blockSize >= 20) {
            const u16 codeBegin = be16(b + 8);
            const u16 codeEnd = be16(b + 10);
            const u16 method = be16(b + 12);
            PL_LOG_INFO("compat.font", "  CMAP: codes 0x%04X..0x%04X (method %u)", codeBegin,
                        codeEnd, method);

            for (u32 code = codeBegin; code <= codeEnd && entries.size() < 512; ++code) {
                CodeEntry e;
                e.code = static_cast<u16>(code);
                e.glyph = -1;
                if (method == 0) {
                    e.glyph = static_cast<int>(code - codeBegin);
                } else if (method == 1) {
                    const u32 infoOff = 20 + 2 * (code - codeBegin);
                    if (infoOff + 2 <= blockSize) {
                        e.glyph = be16(b + infoOff);
                    }
                }
                entries.push_back(e);
            }
        }

        if (blockSize < 8) {
            break;
        }
        off += blockSize;
    }

    // Resolve each glyph index to a sheet cell (NW4R packs the sheets row-major).
    for (CodeEntry& e : entries) {
        if (e.glyph < 0) {
            continue;
        }
        int idx = e.glyph;
        for (size_t s = 0; s < sheets.size(); ++s) {
            const Sheet& sh = sheets[s];
            const int cells = static_cast<int>(sh.gridW) * static_cast<int>(sh.gridH);
            if (cells <= 0) {
                continue;
            }
            if (idx < cells) {
                e.sheet = static_cast<int>(s);
                e.col = idx % sh.gridW;
                e.row = idx / sh.gridW;
                break;
            }
            idx -= cells;
        }
    }

    // Contact sheet: one cell per code, in code order, 4x.
    //
    // NW4R does NOT pack the cells back to back and the sheet has padding on
    // the right/bottom, so width/gridW is NOT the cell stride (it read 42 for a
    // 128x128 sheet with a 3x3 grid of 34x35 cells). The runtime layout is
    // ut_ResFontBase.cpp:GetGlyphFromIndex:
    //
    //     cellX = col * (cellWidth  + 1) + 1
    //     cellY = row * (cellHeight + 1) + 1
    //
    // — a one-texel gutter between cells. The contact sheet follows that, so a
    // window here is exactly what the console samples.
    int cellW = 0;
    int cellH = 0;
    for (const Sheet& s : sheets) {
        if (s.decoded && s.gridW > 0 && s.gridH > 0) {
            cellW = s.cellW;
            cellH = s.cellH;
            break;
        }
    }

    if (cellW > 0 && cellH > 0 && !entries.empty()) {
        const int scale = 4;
        const int gap = 2;
        const int cols = 8;
        const int cw = (cellW + gap) * scale;
        const int ch = (cellH + gap) * scale;
        const int rows = static_cast<int>((entries.size() + cols - 1) / cols);
        const int canvasW = cols * cw;
        const int canvasH = rows * ch;

        std::vector<u8> canvas(static_cast<size_t>(canvasW) * canvasH * 3, 32);
        for (size_t i = 0; i < entries.size(); ++i) {
            const CodeEntry& e = entries[i];
            if (e.sheet < 0) {
                continue;
            }
            const Sheet& s = sheets[e.sheet];
            if (!s.decoded) {
                continue;
            }
            const int gx = static_cast<int>(i % cols) * cw;
            const int gy = static_cast<int>(i / cols) * ch;
            // Same cell origin the runtime computes (1-texel gutters + 1).
            const int originX = e.col * (cellW + 1) + 1;
            const int originY = e.row * (cellH + 1) + 1;
            for (int y = 0; y < cellH * scale; ++y) {
                const int sy = originY + y / scale;
                if (sy >= s.height) {
                    break;
                }
                for (int x = 0; x < cellW * scale; ++x) {
                    const int sx = originX + x / scale;
                    if (sx >= s.width) {
                        break;
                    }
                    const u8* px = &s.rgba[(static_cast<size_t>(sy) * s.width + sx) * 4];
                    const int a = px[3];
                    const int tx = gx + x + gap * scale / 2;
                    const int ty = gy + y + gap * scale / 2;
                    if (tx >= canvasW || ty >= canvasH) {
                        continue;
                    }
                    u8* dst = &canvas[(static_cast<size_t>(ty) * canvasW + tx) * 3];
                    dst[0] = static_cast<u8>((px[0] * a + 96 * (255 - a)) / 255);
                    dst[1] = static_cast<u8>((px[1] * a + 96 * (255 - a)) / 255);
                    dst[2] = static_cast<u8>((px[2] * a + 96 * (255 - a)) / 255);
                }
            }
        }

        const std::string glyphPath = outDir + "picfont_glyphs.ppm";
        const u8* raw = reinterpret_cast<const u8*>(canvas.data());
        if (writePpm(glyphPath, nullptr, canvasW, canvasH, 1, raw, canvasW, canvasH)) {
            PL_LOG_INFO("compat.font", "  -> %s (%dx%d, %u glyph cells, order: code ascending)",
                        glyphPath.c_str(), canvasW, canvasH,
                        static_cast<unsigned>(entries.size()));
        }
    }

    // The text file that ties the contact-sheet positions to the codes.
    const std::string listPath = outDir + "picfont_codes.txt";
    FILE* fp = std::fopen(listPath.c_str(), "wb");
    if (fp != nullptr) {
        std::fprintf(fp,
                     "# picture font dump: one line per glyph cell of picfont_glyphs.ppm\n"
                     "# (8 cells per row, code ascending; each cell is %dx%d texels at 4x,\n"
                     "#  the nw4r layout: sheet origin = col*(w+1)+1, row*(h+1)+1)\n",
                     cellW, cellH);
        for (size_t i = 0; i < entries.size(); ++i) {
            const CodeEntry& e = entries[i];
            std::fprintf(fp,
                         "cell %3u (row %d, col %d): code=0x%04X glyph=%d sheet=%d cell=(%d,%d) "
                         "origin=(%d,%d)\n",
                         static_cast<unsigned>(i), static_cast<int>(i / 8), static_cast<int>(i % 8),
                         e.code, e.glyph, e.sheet, e.col, e.row, e.col * (cellW + 1) + 1,
                         e.row * (cellH + 1) + 1);
        }
        std::fclose(fp);
        PL_LOG_INFO("compat.font", "  -> %s (%u lines)", listPath.c_str(),
                    static_cast<unsigned>(entries.size()));
    }

    PL_LOG_INFO("compat.font", "picture font dump done: %u sheet(s), %u code(s)",
                static_cast<unsigned>(sheets.size()), static_cast<unsigned>(entries.size()));
    return true;
}

}  // namespace compat::ui
