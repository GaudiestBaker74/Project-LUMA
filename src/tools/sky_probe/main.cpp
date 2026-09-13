// =============================================================================
// sky-probe — offline inspector for the title-space sky archive
// (PC_PORT diagnostics; not part of the game runtime).
//
//   sky-probe <archive.rarc> [outDir]
//
// The title backdrop is the J3D model inside ObjectData/CometNearOrbitSky.arc.
// When the composition is off, the question is always the same: which draw item
// is on screen, with which texture, and through which texture-matrix mode? This
// tool answers it without booting the game (no assets tree, no GPU):
//
//   * lists every file in the archive,
//   * loads the .bmd/.bdl through compat/j3d BmdModel and prints one line per
//     draw item: material, shape, joint, mtxType, model bbox, texgens, the tex
//     matrix mode/projection/SRT and the AUTHORED effect matrix (MAT3), cull,
//     blend, z, and the texture it binds,
//   * dumps every TEX1 texture as PPM (RGBA8, alpha dropped) so the images can
//     be compared against the reference frames.
//
// The .rarc must already be decompressed (Yaz0 → RARC); tools/arc_unyaz0.py
// does that in one line.
// =============================================================================

#include "compat/gx/Bti.h"
#include "compat/j3d/BmdModel.h"
#include "compat/j3d/BtkAnim.h"
#include "compat/os/OSCompat.h"
#include "platform/platform.h"

#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#include <JSystem/JKernel/JKRMemArchive.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace compat::j3d;

namespace {

std::vector<u8> readFile(const char* path) {
    std::vector<u8> out;
    FILE* fp = std::fopen(path, "rb");
    if (fp == nullptr) {
        std::fprintf(stderr, "sky-probe: cannot open '%s'\n", path);
        return out;
    }
    std::fseek(fp, 0, SEEK_END);
    const long size = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (size > 0) {
        out.resize(static_cast<size_t>(size));
        if (std::fread(out.data(), 1, out.size(), fp) != out.size()) {
            out.clear();
        }
    }
    std::fclose(fp);
    return out;
}

bool endsWith(const char* s, const char* suffix) {
    const size_t ls = std::strlen(s);
    const size_t lx = std::strlen(suffix);
    if (lx > ls) {
        return false;
    }
    for (size_t i = 0; i < lx; ++i) {
        const char a = s[ls - lx + i];
        const char b = suffix[i];
        const char ca = (a >= 'A' && a <= 'Z') ? static_cast<char>(a - 'A' + 'a') : a;
        const char cb = (b >= 'A' && b <= 'Z') ? static_cast<char>(b - 'A' + 'a') : b;
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

void writePpm(const std::string& path, const u8* rgba, u32 w, u32 h) {
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (fp == nullptr) {
        std::fprintf(stderr, "sky-probe: cannot write '%s'\n", path.c_str());
        return;
    }
    std::fprintf(fp, "P6\n%u %u\n255\n", w, h);
    for (u32 i = 0; i < w * h; ++i) {
        std::fwrite(&rgba[i * 4], 1, 3, fp);
    }
    std::fclose(fp);
    std::printf("  wrote %s (%ux%u)\n", path.c_str(), w, h);
}

}  // namespace

int main(int argc, char** argv) {
    // The archive conversion allocates from the console-RAM arena and logs
    // through the platform layer, so both have to exist first (same prologue
    // as the test binary / the game boot).
    Platform::init();
    compat::initOS();
    Platform::Log::setMinLevel(Platform::Log::Level::Warn);

    if (argc < 2) {
        std::fprintf(stderr, "usage: sky-probe <archive.rarc> [outDir]\n");
        return 2;
    }
    const std::string outDir = (argc > 2) ? argv[2] : ".";

    std::vector<u8> bytes = readFile(argv[1]);
    if (bytes.size() < 0x40) {
        std::fprintf(stderr, "sky-probe: '%s' is empty or too small\n", argv[1]);
        return 1;
    }
    if (std::memcmp(bytes.data(), "Yaz0", 4) == 0) {
        std::fprintf(stderr, "sky-probe: '%s' is Yaz0-compressed — decompress it first "
                             "(tools/arc_unyaz0.py)\n", argv[1]);
        return 1;
    }
    if (std::memcmp(bytes.data(), "RARC", 4) != 0) {
        std::fprintf(stderr, "sky-probe: '%s' is not a RARC archive (magic %.4s)\n", argv[1],
                     reinterpret_cast<const char*>(bytes.data()));
        return 1;
    }

    // JKRMemArchive::open() takes ownership of a buffer that must live inside a
    // JKR heap (it resolves the owning heap with findFromRoot), so the archive
    // is copied into the console-RAM arena first — exactly what the game's
    // mount path does with loadToMainRAM.
    if (JKRHeap::sRootHeap == nullptr) {
        JKRExpHeap::createRoot(1, true);
    }
    if (JKRHeap::sRootHeap == nullptr) {
        std::fprintf(stderr, "sky-probe: cannot create the console-RAM root heap\n");
        return 1;
    }
    JKRHeap::sRootHeap->becomeCurrentHeap();
    JKRHeap* heap = JKRHeap::getCurrentHeap();
    u8* arcBuffer = static_cast<u8*>(JKRHeap::alloc(static_cast<u32>(bytes.size()), 32, heap));
    if (arcBuffer == nullptr) {
        std::fprintf(stderr, "sky-probe: cannot allocate %zu bytes for the archive\n", bytes.size());
        return 1;
    }
    std::memcpy(arcBuffer, bytes.data(), bytes.size());

    JKRMemArchive arc;
    if (!arc.open(arcBuffer, static_cast<u32>(bytes.size()), JKR_MEM_BREAK_FLAG_0)) {
        std::fprintf(stderr, "sky-probe: JKRMemArchive::open failed\n");
        return 1;
    }

    std::printf("archive '%s' (%zu bytes)\n", argv[1], bytes.size());
    std::vector<std::string> names;
    std::vector<std::vector<u8>> blobs;
    for (u32 i = 0;; ++i) {
        JKRArchive::SDirEntry dir;
        if (!arc.getDirEntry(&dir, i)) {
            break;
        }
        if (((dir.mFileFlag >> JKRArchive::FILE_FLAG_FOLDER_SHIFT) & 1) != 0 || dir.mName == nullptr) {
            continue;
        }
        void* res = arc.getIdxResource(i);
        const s32 size = (res != nullptr) ? arc.getResSize(res) : 0;
        names.emplace_back(dir.mName);
        blobs.emplace_back();
        if (res != nullptr && size > 0) {
            blobs.back().assign(static_cast<const u8*>(res), static_cast<const u8*>(res) + size);
        }
        std::printf("  [%u] %-40s %8d bytes\n", i, dir.mName, size);
    }

    // --- the model -----------------------------------------------------------
    const BmdModel* model = nullptr;
    BmdModel loaded;
    for (size_t i = 0; i < names.size(); ++i) {
        if (endsWith(names[i].c_str(), ".bmd") || endsWith(names[i].c_str(), ".bdl")) {
            std::string err;
            if (!loaded.load(blobs[i].data(), blobs[i].size(), &err)) {
                std::fprintf(stderr, "sky-probe: %s: %s\n", names[i].c_str(), err.c_str());
                continue;
            }
            model = &loaded;
            std::printf("\nmodel '%s': %u verts, %zu joints, %zu shapes, %zu materials, %zu textures, "
                        "%zu draw items, loadFlags 0x%x\n",
                        names[i].c_str(), loaded.vertexCount, loaded.joints.size(), loaded.shapes.size(),
                        loaded.materials.size(), loaded.textures.size(), loaded.drawItems.size(),
                        loaded.loadFlags);
            break;
        }
    }
    if (model == nullptr) {
        std::fprintf(stderr, "sky-probe: no .bmd/.bdl in the archive\n");
        return 1;
    }

    for (size_t di = 0; di < model->drawItems.size(); ++di) {
        const BmdDrawItem& it = model->drawItems[di];
        if (it.shape >= model->shapes.size() || it.material >= model->materials.size()) {
            continue;
        }
        const BmdShape& sh = model->shapes[it.shape];
        const BmdMaterial& mt = model->materials[it.material];
        std::printf("draw %zu: mat '%s' shape %zu joint %u mtxType %u groups %zu "
                    "bbox (%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f)\n",
                    di, mt.name.c_str(), static_cast<size_t>(it.shape), static_cast<unsigned>(it.joint),
                    static_cast<unsigned>(sh.mtxType), sh.groups.size(), sh.bboxMin[0], sh.bboxMin[1],
                    sh.bboxMin[2], sh.bboxMax[0], sh.bboxMax[1], sh.bboxMax[2]);
        std::printf("         mode %u cull %u texGens %u", static_cast<unsigned>(mt.mode),
                    static_cast<unsigned>(mt.cullMode), static_cast<unsigned>(mt.texGenNum));
        for (int g = 0; g < mt.texGenNum && g < 8; ++g) {
            std::printf(" tc%d(type %u src %u mtx %u)", g, static_cast<unsigned>(mt.texCoord[g].type),
                        static_cast<unsigned>(mt.texCoord[g].src),
                        static_cast<unsigned>(mt.texCoord[g].mtx));
        }
        std::printf("\n         ");
        for (int s = 0; s < 8; ++s) {
            const BmdTexMtx& tm = mt.texMtx[s];
            if (!tm.valid) {
                continue;
            }
            std::printf("texMtx[%d]: proj %u mode %u maya %d center (%.3f,%.3f,%.3f) srt "
                        "(sx %.3f sy %.3f rot %d tx %.3f ty %.3f) effect ",
                        s, static_cast<unsigned>(tm.projection), static_cast<unsigned>(tm.info & 0x3F),
                        static_cast<int>((tm.info >> 7) & 1), tm.center[0], tm.center[1], tm.center[2],
                        tm.srt.scaleX, tm.srt.scaleY, static_cast<int>(tm.srt.rotation), tm.srt.transX,
                        tm.srt.transY);
            for (int r = 0; r < 4; ++r) {
                std::printf("[%.3f %.3f %.3f %.3f]", tm.effectMtx[r][0], tm.effectMtx[r][1],
                            tm.effectMtx[r][2], tm.effectMtx[r][3]);
            }
            std::printf("\n         ");
        }
        std::printf("texNo:");
        for (int t = 0; t < 8; ++t) {
            if (mt.texNo[t] >= 0) {
                std::printf(" [%d]=%d(%s)", t, static_cast<int>(mt.texNo[t]),
                            static_cast<size_t>(mt.texNo[t]) < model->textures.size()
                                ? model->textures[static_cast<size_t>(mt.texNo[t])].name.c_str()
                                : "?");
            }
        }
        std::printf("         colorChan:");
        for (int c = 0; c < 4; ++c) {
            const BmdColorChan& ch = mt.chan[c];
            std::printf(" [%d] enable=%u ambSrc=%u matSrc=%u lightMask=0x%02x", c,
                        static_cast<unsigned>(ch.enable), static_cast<unsigned>(ch.ambSrc),
                        static_cast<unsigned>(ch.matSrc), static_cast<unsigned>(ch.lightMask));
        }
        std::printf(" amb0=(%u,%u,%u) mat0=(%u,%u,%u)\n", static_cast<unsigned>(mt.ambColor[0][0]),
                    static_cast<unsigned>(mt.ambColor[0][1]), static_cast<unsigned>(mt.ambColor[0][2]),
                    static_cast<unsigned>(mt.matColor[0][0]), static_cast<unsigned>(mt.matColor[0][1]),
                    static_cast<unsigned>(mt.matColor[0][2]));
        std::printf("\n         cull %u blend (%u,%u,%u) z (%u,%u,%u) chan %u matColor (%u,%u,%u,%u) "
                    "tevStages %u\n",
                    static_cast<unsigned>(mt.cullMode), static_cast<unsigned>(mt.blend[0]),
                    static_cast<unsigned>(mt.blend[1]), static_cast<unsigned>(mt.blend[2]),
                    static_cast<unsigned>(mt.zMode[0]), static_cast<unsigned>(mt.zMode[1]),
                    static_cast<unsigned>(mt.zMode[2]), static_cast<unsigned>(mt.chanNum),
                    static_cast<unsigned>(mt.matColor[0][0]), static_cast<unsigned>(mt.matColor[0][1]),
                    static_cast<unsigned>(mt.matColor[0][2]), static_cast<unsigned>(mt.matColor[0][3]),
                    static_cast<unsigned>(mt.tevStageNum));
        for (int st = 0; st < mt.tevStageNum && st < 16; ++st) {
            const BmdTevOrder& o = mt.tevOrder[st];
            const BmdTevStage& g = mt.tevStage[st];
            std::printf("         tev%d: order(tc %d map %d chan %d) kc %u ka %u "
                        "cIn %u,%u,%u,%u op %u bias %u scale %u reg %u clamp %u | "
                        "aIn %u,%u,%u,%u op %u reg %u\n",
                        st, static_cast<int>(o.texCoord), static_cast<int>(o.texMap),
                        static_cast<int>(o.colorChan), static_cast<unsigned>(mt.kColorSel[st]),
                        static_cast<unsigned>(mt.kAlphaSel[st]), static_cast<unsigned>(g.colorIn[0]),
                        static_cast<unsigned>(g.colorIn[1]), static_cast<unsigned>(g.colorIn[2]),
                        static_cast<unsigned>(g.colorIn[3]), static_cast<unsigned>(g.colorOp),
                        static_cast<unsigned>(g.colorBias), static_cast<unsigned>(g.colorScale),
                        static_cast<unsigned>(g.colorReg), static_cast<unsigned>(g.colorClamp),
                        static_cast<unsigned>(g.alphaIn[0]), static_cast<unsigned>(g.alphaIn[1]),
                        static_cast<unsigned>(g.alphaIn[2]), static_cast<unsigned>(g.alphaIn[3]),
                        static_cast<unsigned>(g.alphaOp), static_cast<unsigned>(g.alphaReg));
        }
        std::printf("         tevColor:");
        for (int c = 0; c < 3; ++c) {
            std::printf(" R%d(%d,%d,%d,%d)", c, mt.tevColor[c][0], mt.tevColor[c][1], mt.tevColor[c][2],
                        mt.tevColor[c][3]);
        }
        std::printf(" kColor:");
        for (int c = 0; c < 4; ++c) {
            std::printf(" K%d(%u,%u,%u,%u)", c, static_cast<unsigned>(mt.kColor[c][0]),
                        static_cast<unsigned>(mt.kColor[c][1]), static_cast<unsigned>(mt.kColor[c][2]),
                        static_cast<unsigned>(mt.kColor[c][3]));
        }
        std::printf("\n");
    }

    // --- UV parameterisation of the earth band (shape 4/5 = the sea) ---------
    {
        const BmdVertexArray* pos = model->array(GX_VA_POS);
        const BmdVertexArray* tex0 = model->array(GX_VA_TEX0);
        if (pos != nullptr && tex0 != nullptr && pos->compType == GX_F32) {
            const size_t esz = pos->data.size() / (pos->elementCount ? pos->elementCount : 1);
            const size_t tsz = tex0->data.size() / (tex0->elementCount ? tex0->elementCount : 1);
            const auto posAt = [&](size_t i, int k) {
                f32 v;
                std::memcpy(&v, pos->data.data() + i * esz + static_cast<size_t>(k) * 4, 4);
                return v;
            };
            const auto uvAt = [&](size_t i, int k) {
                s16 v;
                std::memcpy(&v, tex0->data.data() + i * tsz + static_cast<size_t>(k) * 2, 2);
                return static_cast<f32>(v) / static_cast<f32>(1 << tex0->frac);
            };
            {
                // Full POS.y -> raw TEX0 table for the earth dome (shape 4/5):
                // lets the sea's texcoord layout be compared against the
                // reference frame without booting the game.
                std::printf("\nearth dome raw attrs (y_model, u, v) by height:\n");
                std::vector<size_t> dome;
                for (size_t i = 0; i < pos->elementCount; ++i) {
                    if (posAt(i, 1) < 20000.0f) dome.push_back(i);
                }
                std::sort(dome.begin(), dome.end(),
                          [&](size_t a, size_t b) { return posAt(a, 1) < posAt(b, 1); });
                for (size_t k = 0; k < dome.size(); k += 12) {
                    const size_t i = dome[k];
                    std::printf("    y=%+10.0f  tex0=(%8.4f,%8.4f)\n", posAt(i, 1),
                                uvAt(i, 0), uvAt(i, 1));
                }
                std::printf("  (%zu verts below y=20000)\n", dome.size());
            }
            std::printf("\nearth band (POS.y in {-17190, -331847}), sorted by z:\n");
            for (f32 ringY : {-331847.0f, -17190.0f}) {
                std::vector<size_t> ring;
                for (size_t i = 0; i < pos->elementCount; ++i) {
                    if (std::fabs(posAt(i, 1) - ringY) < 60.0f) {
                        ring.push_back(i);
                    }
                }
                std::sort(ring.begin(), ring.end(), [&](size_t a, size_t b) { return posAt(a, 2) < posAt(b, 2); });
                std::printf("  ring y=%+9.0f: %zu verts\n", ringY, ring.size());
                const size_t step = ring.size() > 12 ? ring.size() / 12 : 1;
                for (size_t k = 0; k < ring.size(); k += step) {
                    const size_t i = ring[k];
                    std::printf("    x=%+9.0f z=%+9.0f   u=%8.4f v=%8.4f  (tile u=%+6.3f v=%+6.3f)\n",
                                posAt(i, 0), posAt(i, 2), uvAt(i, 0), uvAt(i, 1),
                                uvAt(i, 0) - std::floor(uvAt(i, 0)), uvAt(i, 1) - std::floor(uvAt(i, 1)));
                }
            }
        }
    }

    // --- vertex attributes (is the TEX0 the sea samples sane?) ---------------
    for (u32 a : {GX_VA_POS, GX_VA_NRM, GX_VA_CLR0, GX_VA_CLR1, GX_VA_TEX0}) {
        const BmdVertexArray* arr = model->array(a);
        if (arr == nullptr || !arr->valid()) {
            std::printf("attr 0x%02x: (absent)\n", a);
            continue;
        }
        // GX_POS_XY and GX_TEX_ST are both 0/1 in different enums, so the
        // component count depends on which attribute this is.
        const bool isVec3 = (a == GX_VA_POS || a == GX_VA_NRM);
        const int comps = isVec3 ? (arr->compCnt == GX_POS_XY ? 2 : 3)
                                 : (arr->compCnt == GX_TEX_ST ? 2 : 1);
        const size_t n = arr->elementCount;
        // Only the float component types are dumped as-is; the fixed-point ones
        // (s16/u16 texcoords, s8 normals) are stored host-endian but not
        // converted, so a range over them would be meaningless here.
        const bool isFloat = (arr->compType == GX_F32);
        // Fixed-point attributes are stored host-endian but scaled only at
        // read time (value / 2^frac), so decode them here to report a range.
        const auto comp = [&](size_t el, int k) -> f32 {
            const size_t esz2 = arr->data.size() / (n ? n : 1);
            const u8* p = arr->data.data() + el * esz2 + static_cast<size_t>(k) * 2;
            s16 v;
            std::memcpy(&v, p, sizeof(v));
            return static_cast<f32>(v) / static_cast<f32>(1 << arr->frac);
        };
        f32 lo[3] = {1e30f, 1e30f, 1e30f};
        f32 hi[3] = {-1e30f, -1e30f, -1e30f};
        const size_t esz = arr->data.size() / (n ? n : 1);
        for (size_t i = 0; i < n; ++i) {
            const f32* v = reinterpret_cast<const f32*>(arr->data.data() + i * esz);
            for (int k = 0; k < comps && k < 3; ++k) {
                lo[k] = std::min(lo[k], v[k]);
                hi[k] = std::max(hi[k], v[k]);
            }
        }
        if (isFloat) {
            std::printf("attr 0x%02x: %zu elements, %d comps, stride %u, compType F32, range "
                        "(%.4f,%.4f,%.4f)-(%.4f,%.4f,%.4f)\n",
                        a, n, comps, static_cast<unsigned>(arr->stride), lo[0], lo[1], lo[2], hi[0],
                        hi[1], hi[2]);
        } else {
            f32 lo2[2] = {1e30f, 1e30f};
            f32 hi2[2] = {-1e30f, -1e30f};
            for (size_t i = 0; i < n; ++i) {
                for (int k = 0; k < comps && k < 2; ++k) {
                    const f32 v = comp(i, k);
                    lo2[k] = std::min(lo2[k], v);
                    hi2[k] = std::max(hi2[k], v);
                }
            }
            std::printf("attr 0x%02x: %zu elements, %d comps, stride %u, compType 0x%x frac %u, range "
                        "(%.4f,%.4f)-(%.4f,%.4f)\n",
                        a, n, comps, static_cast<unsigned>(arr->stride),
                        static_cast<unsigned>(arr->compType), static_cast<unsigned>(arr->frac), lo2[0],
                        lo2[1], hi2[0], hi2[1]);
        }
    }
    for (size_t si = 0; si < model->shapes.size(); ++si) {
        const BmdShape& sh = model->shapes[si];
        std::printf("shape %zu: %zu decls:", si, sh.decls.size());
        for (const BmdVtxDecl& dcl : sh.decls) {
            std::printf(" attr0x%02x", dcl.attr);
        }
        std::printf("\n");
    }

    // --- texture-SRT animation (which materials' tex matrices move) ----------
    for (size_t i = 0; i < names.size(); ++i) {
        if (!endsWith(names[i].c_str(), ".btk")) {
            continue;
        }
        BtkAnim btk;
        std::string err;
        if (!btk.load(blobs[i].data(), blobs[i].size(), &err)) {
            std::printf("\nbtk '%s': %s\n", names[i].c_str(), err.c_str());
            continue;
        }
        std::printf("\nbtk '%s': %zu entries, duration %u, loop %u, maya %d, rotDecShift %u\n",
                    names[i].c_str(), btk.entries.size(), static_cast<unsigned>(btk.duration),
                    static_cast<unsigned>(btk.loopMode), btk.maya ? 1 : 0,
                    static_cast<unsigned>(btk.rotDecShift));
        for (size_t e = 0; e < btk.entries.size(); ++e) {
            const BtkEntry& en = btk.entries[e];
            std::printf("  entry %zu: material '%s' slot %u center (%.2f,%.2f,%.2f)\n", e,
                        en.materialName.c_str(), static_cast<unsigned>(en.texMtxSlot), en.center[0],
                        en.center[1], en.center[2]);
            for (int f : {0, 1250, 2500, 5000, 7500}) {
                TexSrt srt;
                btk.evaluate(e, static_cast<f32>(f), srt);
                std::printf("    frame %5d: sx %8.3f sy %8.3f rot %6d tx %8.3f ty %8.3f\n", f,
                            srt.scaleX, srt.scaleY, static_cast<int>(srt.rotation), srt.transX,
                            srt.transY);
            }
        }
    }

    // --- textures ------------------------------------------------------------
    for (size_t i = 0; i < names.size(); ++i) {
        if (!endsWith(names[i].c_str(), ".bti") || blobs[i].size() < 32) {
            continue;
        }
        const u8* base = blobs[i].data();
        Platform::CompatGx::BtiHeader hdr;
        if (!Platform::CompatGx::btiParseHeader(base, blobs[i].size(), hdr)) {
            std::printf("texture '%s': bad BTI header\n", names[i].c_str());
            continue;
        }
        const u8* image = base + hdr.imageOffset;
        const size_t imageBytes = Platform::CompatGx::btiImageSize(hdr.width, hdr.height, hdr.format);
        const u8* palette = hdr.palettesEnabled ? base + hdr.paletteOffset : nullptr;
        size_t paletteBytes = 0;
        if (palette != nullptr) {
            paletteBytes = static_cast<size_t>(hdr.paletteCount) * 2;  // TLUT entries are 2 bytes
        }
        std::vector<u8> rgba(static_cast<size_t>(hdr.width) * hdr.height * 4);
        std::printf("texture '%s': %ux%u fmt 0x%x wrap (%u,%u) palettes %u\n", names[i].c_str(),
                    static_cast<unsigned>(hdr.width), static_cast<unsigned>(hdr.height),
                    static_cast<unsigned>(hdr.format), static_cast<unsigned>(hdr.wrapS),
                    static_cast<unsigned>(hdr.wrapT), static_cast<unsigned>(hdr.palettesEnabled));
        if (!Platform::CompatGx::btiDecodeToRgba8(image, imageBytes, hdr.width, hdr.height, hdr.format,
                                                  palette, paletteBytes, hdr.paletteFormat, rgba.data())) {
            std::printf("  (decode failed)\n");
            continue;
        }
        char path[512];
        std::snprintf(path, sizeof(path), "%s/%s.ppm", outDir.c_str(), names[i].c_str());
        writePpm(path, rgba.data(), hdr.width, hdr.height);
    }

    // Textures may also live inside the model's TEX1 block (no .bti files).
    for (size_t ti = 0; ti < model->textures.size(); ++ti) {
        const BmdTexture& tx = model->textures[ti];
        std::printf("model texture %zu '%s': %ux%u fmt 0x%x wrap (%u,%u) mip %u image %zu bytes\n", ti,
                    tx.name.c_str(), static_cast<unsigned>(tx.header.width),
                    static_cast<unsigned>(tx.header.height), static_cast<unsigned>(tx.header.format),
                    static_cast<unsigned>(tx.header.wrapS), static_cast<unsigned>(tx.header.wrapT),
                    static_cast<unsigned>(tx.mipmap), tx.imageBytes);
        if (tx.image == nullptr) {
            continue;
        }
        std::vector<u8> rgba(static_cast<size_t>(tx.header.width) * tx.header.height * 4);
        if (!Platform::CompatGx::btiDecodeToRgba8(tx.image, tx.imageBytes, tx.header.width,
                                                  tx.header.height, tx.header.format, tx.palette,
                                                  tx.paletteBytes, tx.header.paletteFormat,
                                                  rgba.data())) {
            std::printf("  (decode failed)\n");
            continue;
        }
        char path[512];
        std::snprintf(path, sizeof(path), "%s/model_tex%zu_%s.ppm", outDir.c_str(), ti, tx.name.c_str());
        writePpm(path, rgba.data(), tx.header.width, tx.header.height);
    }

    Platform::shutdown();
    return 0;
}
