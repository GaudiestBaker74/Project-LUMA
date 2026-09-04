// =============================================================================
// make_synth_strap — genera assets/LayoutData/WiiRemoteStrap.arc sintético.
//
// Herramienta de diagnóstico (no va en el CMake): construye un RARC con la
// misma forma que el arc real del juego (mismos nombres de pane, mismos
// tamaños de textura y formatos que se ven en el dump del boot del usuario)
// para reproducir el arranque real sin assets propietarios:
//
//   wiiremotestrapreplace.brlyt  RootPane(608x456, basePos4)
//                                  > pic1  PicBG   608x456  tex 8x8   I4
//                                  > pan1  WiiRemoteStrap 30x40
//                                    > pan1 PlayMessage 30x40
//                                      > pic1 PicPlay 568x392 pos(2,0) RGB565
//                                    > pan1 HookMessage (oculto)
//                                      > pic1 PicHook 360x256 pos(106,20)
//                                              tex[0] 360x256 RGB565
//                                              tex[1] 40x40   I4
//   strap.brlan                  RootPane RLVC PaneAlpha 255 constante,
//                                frameSize 600, loop (como el anim real)
//
// Uso:  make_synth_strap [salida.arc]
// =============================================================================
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef float f32;

static void put8(std::vector<u8>& v, u8 x) { v.push_back(x); }
static void put16(std::vector<u8>& v, u16 x) { v.push_back(u8(x >> 8)); v.push_back(u8(x)); }
static void put32(std::vector<u8>& v, u32 x) {
    v.push_back(u8(x >> 24)); v.push_back(u8(x >> 16)); v.push_back(u8(x >> 8)); v.push_back(u8(x));
}
static void putF32(std::vector<u8>& v, f32 f) {
    u32 bits; std::memcpy(&bits, &f, 4); put32(v, bits);
}
static void putBytes(std::vector<u8>& v, const void* p, size_t n) {
    const u8* b = static_cast<const u8*>(p);
    v.insert(v.end(), b, b + n);
}
static void putFixedStr(std::vector<u8>& v, const char* s, size_t fixedLen) {
    const size_t n = std::strlen(s);
    putBytes(v, s, n < fixedLen ? n : fixedLen);
    for (size_t i = n; i < fixedLen; ++i) put8(v, 0);
}
static void putNulStr(std::vector<u8>& v, const char* s) { putBytes(v, s, std::strlen(s) + 1); }
static void padTo4(std::vector<u8>& v) { while (v.size() % 4) put8(v, 0); }
static void patch32(std::vector<u8>& v, size_t off, u32 x) {
    v[off] = u8(x >> 24); v[off + 1] = u8(x >> 16); v[off + 2] = u8(x >> 8); v[off + 3] = u8(x);
}

// --- brlyt ------------------------------------------------------------------

struct BrlytBuilder {
    std::vector<u8> blocks;
    int blockCount = 0;

    u32 beginBlock(const char* kind) {
        const u32 off = static_cast<u32>(16 + blocks.size());
        putBytes(blocks, kind, 4);
        put32(blocks, 0);
        return off;
    }
    void endBlock(u32 blockOff) {
        const u32 size = static_cast<u32>(16 + blocks.size()) - blockOff;
        patch32(blocks, blockOff + 4 - 16, size);
        ++blockCount;
    }
    std::vector<u8>& raw() { return blocks; }
    std::vector<u8> finish() {
        std::vector<u8> f;
        putBytes(f, "RLYT", 4);
        put16(f, 0xFEFF);
        put16(f, 0x000A);
        put32(f, static_cast<u32>(16 + blocks.size()));
        put16(f, 16);
        put16(f, static_cast<u16>(blockCount));
        putBytes(f, blocks.data(), blocks.size());
        return f;
    }
};

// res::Pane en disco: 76 bytes. basePos 4 = centro-centro (como el arc real).
static void putPaneBody(std::vector<u8>& v, const char* name, f32 tx, f32 ty, u8 basePos, u8 flag,
                        f32 w, f32 h) {
    put8(v, flag);      // flag (bit0 visible)
    put8(v, basePos);   // basePosition
    put8(v, 0xFF);      // alpha
    put8(v, 0);
    putFixedStr(v, name, 16);
    putFixedStr(v, "", 8);  // userData
    putF32(v, tx); putF32(v, ty); putF32(v, 0.0f);  // translate
    putF32(v, 0.0f); putF32(v, 0.0f); putF32(v, 0.0f);  // rotate
    putF32(v, 1.0f); putF32(v, 1.0f);  // scale
    putF32(v, w); putF32(v, h);        // size
}

// TevStage (16 B) preset REPLACE: out.rgb = TEXC, out.a = TEXA (idéntico al
// del test lyt_layout, que ya valida la ruta de dibujo).
static const u8 kTevStage[16] = {0, 4, 0, 0, 0xFF, 0x8F, 0x00, 0x61, 0x77, 0x47, 0x00, 0x81, 0, 0, 0, 0};

struct MatDef {
    const char* name;
    u16 tex0;  // índice txl1 del texmap 0
    u16 tex1;  // 0xFFFF = solo 1 texmap
};

// mat1: N materiales. resNum = texMap n | texSRT 1<<4 | texCoordGen 1<<8 |
//        tevStage 1<<18. Tail: TexMaps (4 B c/u) + TexSRT(20) + TexCoordGen(4)
//        + TevStage(16).
static void putMaterial(std::vector<u8>& v, const MatDef& m) {
    putFixedStr(v, m.name, 20);
    for (int i = 0; i < 3; ++i) {
        for (int c = 0; c < 4; ++c) put16(v, 255);  // tevCols: blanco
    }
    for (int i = 0; i < 16; ++i) put8(v, 0);  // tevKCols
    const u32 nTex = (m.tex1 == 0xFFFF) ? 1u : 2u;
    put32(v, nTex | (1u << 4) | (1u << 8) | (1u << 18));
    put16(v, m.tex0); put8(v, 0); put8(v, 0);
    if (nTex == 2) { put16(v, m.tex1); put8(v, 0); put8(v, 0); }
    putF32(v, 0.0f); putF32(v, 0.0f);  // TexSRT.translate
    putF32(v, 0.0f);                   // TexSRT.rotate
    putF32(v, 1.0f); putF32(v, 1.0f);  // TexSRT.scale
    put8(v, 1); put8(v, 4); put8(v, 60); put8(v, 0);  // TexCoordGen MTX2x4/TEX0/IDENTITY
    for (int i = 0; i < 16; ++i) put8(v, kTevStage[i]);
}

struct PaneDef {
    const char* kind;    // "pan1" | "pic1"
    const char* name;
    f32 tx, ty;
    u8 basePos;
    u8 flag;
    f32 w, h;
    int matIdx;  // -1 para pan1
};

// Genera el brlyt completo (mismo orden de bloques que el dump del usuario:
// lyt1 txl1 mat1 [pan1 pas1 pic1]... grp1).
static std::vector<u8> makeStrapBrlyt() {
    const MatDef mats[4] = {
        {"mat_bg", 0, 0xFFFF},
        {"mat_play", 1, 0xFFFF},
        {"mat_hook", 2, 3},
        {"mat_cur", 3, 0xFFFF},
    };
    const PaneDef panes[] = {
        {"pan1", "RootPane", 0.0f, 0.0f, 4, 1, 608.0f, 456.0f, -1},
        {"pic1", "PicBG", 0.0f, 0.0f, 4, 1, 608.0f, 456.0f, 0},
        {"pan1", "WiiRemoteStrap", 0.0f, 0.0f, 4, 1, 30.0f, 40.0f, -1},
        {"pan1", "PlayMessage", 0.0f, 0.0f, 4, 1, 30.0f, 40.0f, -1},
        {"pic1", "PicPlay", 2.0f, 0.0f, 4, 1, 568.0f, 392.0f, 1},
        {"pan1", "HookMessage", 0.0f, 0.0f, 4, 0, 30.0f, 40.0f, -1},
        {"pic1", "PicHook", 106.0f, 20.0f, 4, 1, 360.0f, 256.0f, 2},
    };

    BrlytBuilder b;
    // lyt1 (20)
    const u32 lyt1 = b.beginBlock("lyt1");
    put8(b.raw(), 0); put8(b.raw(), 0); put8(b.raw(), 0); put8(b.raw(), 0);
    putF32(b.raw(), 608.0f); putF32(b.raw(), 456.0f);
    b.endBlock(lyt1);

    // txl1: 4 texturas. Formato real nw4r: tras el header (8) vienen
    // texNum(2)+pad(2) y N entradas res::Texture de 8 bytes
    // {u32 nameStrOffset; u8 type; u8 pad[3]}; nameStrOffset es RELATIVO AL
    // ARRAY DE ENTRADAS (bloque+12), como consume lyt_material.cpp
    // (ConvertOffsToPtr(textures, nameStrOffset)).
    const char* texNames[4] = {"bg.tpl", "play.tpl", "hook.tpl", "cursor.tpl"};
    const u32 txl1 = b.beginBlock("txl1");
    {
        std::vector<u8>& v = b.raw();
        put16(v, 4); put16(v, 0);
        // Strings tras las 4 entradas: bloque+12+32 = bloque+44.
        u32 strOff = 44;
        u32 entryRel[4];
        for (int i = 0; i < 4; ++i) {
            entryRel[i] = strOff - 12;  // relativo al array de entradas
            const u32 len = static_cast<u32>(std::strlen(texNames[i])) + 1;
            strOff += (len + 3u) & ~3u;
        }
        for (int i = 0; i < 4; ++i) {
            put32(v, entryRel[i]);
            put8(v, 0); put8(v, 0); put8(v, 0); put8(v, 0);  // type + pad
        }
        for (int i = 0; i < 4; ++i) {
            putNulStr(v, texNames[i]);
            padTo4(v);
        }
    }
    b.endBlock(txl1);

    // mat1: 4 materiales.
    const u32 mat1 = b.beginBlock("mat1");
    {
        std::vector<u8>& v = b.raw();
        put16(v, 4); put16(v, 0);
        u32 matOff = 28;
        u32 offs[4];
        for (int i = 0; i < 4; ++i) {
            offs[i] = matOff;
            put32(v, matOff);
            const u32 nTex = (mats[i].tex1 == 0xFFFF) ? 1u : 2u;
            matOff += 104u + 4u * nTex;
        }
        for (int i = 0; i < 4; ++i) {
            putMaterial(v, mats[i]);
        }
    }
    b.endBlock(mat1);

    // Panes + jerarquía.
    auto emitPane = [&](const PaneDef& p) {
        const u32 blk = b.beginBlock(p.kind);
        putPaneBody(b.raw(), p.name, p.tx, p.ty, p.basePos, p.flag, p.w, p.h);
        if (std::strcmp(p.kind, "pic1") == 0) {
            std::vector<u8>& v = b.raw();
            for (int i = 0; i < 4; ++i) put32(v, 0xFFFFFFFF);  // vtxCols blancos
            put16(v, static_cast<u16>(p.matIdx));
            put8(v, 1);  // texCoordNum
            put8(v, 0);
            const f32 tc[8] = {0, 0, 1, 0, 1, 1, 0, 1};
            for (int i = 0; i < 8; ++i) putF32(v, tc[i]);
        }
        b.endBlock(blk);
    };

    // RootPane
    emitPane(panes[0]);
    { const u32 x = b.beginBlock("pas1"); b.endBlock(x); }          // hijos RootPane
    emitPane(panes[1]);                                             // PicBG
    emitPane(panes[2]);                                             // WiiRemoteStrap
    { const u32 x = b.beginBlock("pas1"); b.endBlock(x); }          // hijos WRS
    emitPane(panes[3]);                                             // PlayMessage
    { const u32 x = b.beginBlock("pas1"); b.endBlock(x); }          // hijos PlayMessage
    emitPane(panes[4]);                                             // PicPlay (mat 1)
    { const u32 x = b.beginBlock("pae1"); b.endBlock(x); }          // fin PlayMessage
    emitPane(panes[5]);                                             // HookMessage
    { const u32 x = b.beginBlock("pas1"); b.endBlock(x); }          // hijos HookMessage
    emitPane(panes[6]);                                             // PicHook (mat 2)

    { const u32 x = b.beginBlock("pae1"); b.endBlock(x); }          // fin HookMessage
    { const u32 x = b.beginBlock("pae1"); b.endBlock(x); }          // fin WRS
    { const u32 x = b.beginBlock("pae1"); b.endBlock(x); }          // fin RootPane

    // grp1 raíz (28 bytes, como el dump).
    const u32 grp1 = b.beginBlock("grp1");
    putFixedStr(b.raw(), "WiiRemoteStrapRoot", 16);
    put16(b.raw(), 0); put16(b.raw(), 0);
    b.endBlock(grp1);

    return b.finish();
}

// --- TPL --------------------------------------------------------------------

// Codifica un nivel GX tiled: fmt 0 = I4, fmt 4 = RGB565.
static void encodeLevel(const std::vector<u8>& rgba, u32 w, u32 h, u8 fmt, std::vector<u8>& out) {
    const u32 tilesW = w / 4, tilesH = h / 4;
    if (fmt == 0) {  // I4
        out.resize(tilesW * tilesH * 8);
        for (u32 y = 0; y < h; ++y) {
            for (u32 x = 0; x < w; ++x) {
                const u8* p = &rgba[(y * w + x) * 4];
                const u8 lum = static_cast<u8>((p[0] + p[1] + p[2]) / 3);
                const u8 nib = lum >> 4;
                u8& byte = out[((y / 4) * tilesW + (x / 4)) * 8 + (y % 4) * 2 + (x % 4) / 2];
                if ((x % 4) % 2 == 0) {
                    byte = static_cast<u8>((byte & 0x0F) | (nib << 4));
                } else {
                    byte = static_cast<u8>((byte & 0xF0) | nib);
                }
            }
        }
    } else {  // RGB565
        out.resize(tilesW * tilesH * 32);
        for (u32 y = 0; y < h; ++y) {
            for (u32 x = 0; x < w; ++x) {
                const u8* p = &rgba[(y * w + x) * 4];
                const u16 c = static_cast<u16>(((p[0] * 31u + 127u) / 255u) << 11 |
                                               ((p[1] * 63u + 127u) / 255u) << 5 |
                                               ((p[2] * 31u + 127u) / 255u));
                const size_t o = ((y / 4) * tilesW + (x / 4)) * 32 + ((y % 4) * 4 + (x % 4)) * 2;
                out[o] = static_cast<u8>(c >> 8);
                out[o + 1] = static_cast<u8>(c);
            }
        }
    }
}

// TPL BE con 1 descriptor (sin CLUT, sin mips).
static std::vector<u8> makeTpl(u32 w, u32 h, u8 fmt, const std::vector<u8>& rgba) {
    std::vector<u8> texels;
    encodeLevel(rgba, w, h, fmt, texels);

    std::vector<u8> v;
    put32(v, 0x0020AF30);
    put32(v, 1);
    put32(v, 12);       // descriptor @12
    put32(v, 20);       // texture header @20
    put32(v, 0);        // sin paleta
    put16(v, static_cast<u16>(h));
    put16(v, static_cast<u16>(w));
    put32(v, fmt);
    put32(v, 56);       // data @56 (header 36 bytes)
    put32(v, 0); put32(v, 0);   // wrap clamp
    put32(v, 0); put32(v, 0);   // filters nearest (el juego usa point para UI)
    putF32(v, 0.0f);
    put8(v, 0); put8(v, 0); put8(v, 0); put8(v, 0);
    putBytes(v, texels.data(), texels.size());
    return v;
}

static std::vector<u8> solidRgba(u32 w, u32 h, u8 r, u8 g, u8 b) {
    std::vector<u8> v(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < v.size(); i += 4) {
        v[i] = r; v[i + 1] = g; v[i + 2] = b; v[i + 3] = 255;
    }
    return v;
}

// --- brlan ------------------------------------------------------------------

static std::vector<u8> makeHermiteKeys(const f32 (*keys)[3], int num) {
    std::vector<u8> v;
    for (int i = 0; i < num; ++i) {
        putF32(v, keys[i][0]); putF32(v, keys[i][1]); putF32(v, keys[i][2]);
    }
    return v;
}

// Claves RLTP: tipo 1 = {f32 frame; u16 texIdx; u16 pad}.
static std::vector<u8> makeRltpKeys() {
    std::vector<u8> v;
    putF32(v, 0.0f);   put16(v, 0); put16(v, 0);  // frame 0 -> play.tpl
    putF32(v, 100.0f); put16(v, 1); put16(v, 0);  // frame 100 -> alt.tpl
    return v;
}

static std::vector<u8> makeStepKeys(const f32 (*frames)[1], const u16* values, int num) {
    std::vector<u8> v;
    for (int i = 0; i < num; ++i) {
        putF32(v, frames[i][0]);
        put16(v, values[i]);
        put16(v, 0);  // pad
    }
    return v;
}

static std::vector<u8> makeBrlanEntry(u8 index, u8 target, u8 keyType, u16 keyNum,
                                      const std::vector<u8>& keys) {
    std::vector<u8> e;
    put8(e, index); put8(e, target); put8(e, keyType); put8(e, 0);
    put16(e, keyNum); put16(e, 0);
    put32(e, 0x0C);
    e.insert(e.end(), keys.begin(), keys.end());
    return e;
}

static std::vector<u8> makeBrlanTag(const char* kind, const std::vector<std::vector<u8>>& entries) {
    std::vector<u8> t;
    putBytes(t, kind, 4);
    put8(t, static_cast<u8>(entries.size()));
    put8(t, 0); put8(t, 0); put8(t, 0);
    std::vector<size_t> patches;
    for (size_t i = 0; i < entries.size(); ++i) {
        patches.push_back(t.size());
        put32(t, 0);
    }
    for (size_t i = 0; i < entries.size(); ++i) {
        patch32(t, patches[i], static_cast<u32>(t.size()));
        t.insert(t.end(), entries[i].begin(), entries[i].end());
    }
    return t;
}

static std::vector<u8> makeBrlanContent(const char* name, u8 targetKind,
                                        const std::vector<std::vector<u8>>& tags) {
    std::vector<u8> c;
    putFixedStr(c, name, 0x14);
    put8(c, static_cast<u8>(tags.size()));
    put8(c, targetKind);
    put16(c, 0);
    std::vector<size_t> patches;
    for (size_t i = 0; i < tags.size(); ++i) {
        patches.push_back(c.size());
        put32(c, 0);
    }
    for (size_t i = 0; i < tags.size(); ++i) {
        patch32(c, patches[i], static_cast<u32>(c.size()));
        c.insert(c.end(), tags[i].begin(), tags[i].end());
    }
    return c;
}

// strap.brlan: frameSize 600 loop. Dos contents:
//   RootPane:     RLVC PaneAlpha 255 constante (2 keys planos).
//   HookMessage:  RLVI Visibility STEP: 0 hasta el frame 270, 1 desde 270 —
//                 replica al arc real, donde la animación activa el cartel
//                 del mando en horizontal a mitad del anim (M9.5.3c: el
//                 usuario crasheaba ~frame 300 == anim frame ~270, justo el
//                 PRIMER draw de PicHook, con material de 2 texturas).
static std::vector<u8> makeStrapBrlan() {
    const f32 alpha[2][3] = {{0.0f, 255.0f, 0.0f}, {600.0f, 255.0f, 0.0f}};
    std::vector<std::vector<u8>> rlvc;
    rlvc.push_back(makeBrlanEntry(0, 0x10, 2, 2, makeHermiteKeys(alpha, 2)));
    std::vector<std::vector<u8>> paneTags;
    paneTags.push_back(makeBrlanTag("RLVC", rlvc));
    std::vector<u8> contentRoot = makeBrlanContent("RootPane", 0, paneTags);

    const f32 visFrames[2][1] = {{0.0f}, {270.0f}};
    const u16 visValues[2] = {0, 1};
    std::vector<std::vector<u8>> rlviHook;
    rlviHook.push_back(makeBrlanEntry(0, 0x00, 1, 2, makeStepKeys(visFrames, visValues, 2)));
    std::vector<std::vector<u8>> hookTags;
    hookTags.push_back(makeBrlanTag("RLVI", rlviHook));
    std::vector<u8> contentHook = makeBrlanContent("HookMessage", 0, hookTags);

    // PicPlay (contenido de PANE, targetKind 0) con RLTP: la ruta que el
    // runtime ignoraba (tags de material bajo contenido de pane). En el
    // frame 100 cambia texMap0 de play.tpl (idx 0) a alt.tpl (idx 1, verde).
    std::vector<std::vector<u8>> rltpEntries;
    rltpEntries.push_back(makeBrlanEntry(0, 0x00, 1, 2,
                                         makeRltpKeys()));  // frames 0->tex0, 100->tex1
    std::vector<std::vector<u8>> playTags;
    playTags.push_back(makeBrlanTag("RLTP", rltpEntries));
    std::vector<u8> contentPlay = makeBrlanContent("PicPlay", 0, playTags);

    // pai1 body (layout real del formato):
    //   blk+0x08 frameSize/loop/pad | 0x0C fileNum | 0x0E animContNum
    //   blk+0x10 animContOffsetsOffset u32 (bloque-relativo)
    //   blk+0x14 u32 fileOffsets[fileNum]  -> nombres RELATIVOS AL ARRAY
    //   a continuacion: pool de strings, array de contenidos, contents.
    std::vector<u8> body;
    put16(body, 600);  // frameSize
    put8(body, 1);     // loop
    put8(body, 0);
    put16(body, 2);    // fileNum (tabla: play.tpl, alt.tpl)
    put16(body, 3);    // animContNum
    // (animContOffsetsOffset se parchea cuando sepamos donde queda el array)
    put32(body, 0);
    // Tabla de ficheros: 2 entradas; offsets RELATIVOS AL INICIO DEL ARRAY
    // (GetStrTableStr: &pool[offs[i]]). Pool detras del array, explícito:
    //   array en body+0x0C: [4][13] ; pool en body+0x14: "play.tpl\0alt.tpl\0"
    put32(body, 8);   // -> "play.tpl" (idx 0): el pool empieza en array+8
    put32(body, 17);  // -> "alt.tpl"  (idx 1): 8 + 9 ("play.tpl\0")
    for (const char* q = "play.tpl"; *q; ++q) {
        put8(body, static_cast<u8>(*q));
    }
    put8(body, 0);
    for (const char* q = "alt.tpl"; *q; ++q) {
        put8(body, static_cast<u8>(*q));
    }
    put8(body, 0);
    while ((8 + body.size()) % 4) {
        put8(body, 0);
    }

    // Array de offsets de contenidos (3 u32, bloque-relativo).
    const size_t contOffsBodyPos = body.size();
    const u32 contArrayOff = static_cast<u32>(8 + contOffsBodyPos);
    patch32(body, 0x08, contArrayOff);  // animContOffsetsOffset vive en bloque+0x10 = body+0x08
    put32(body, 0);
    put32(body, 0);
    put32(body, 0);
    const u32 content0Off = static_cast<u32>(8 + body.size());
    body.insert(body.end(), contentRoot.begin(), contentRoot.end());
    const u32 content1Off = static_cast<u32>(8 + body.size());
    body.insert(body.end(), contentHook.begin(), contentHook.end());
    const u32 content2Off = static_cast<u32>(8 + body.size());
    body.insert(body.end(), contentPlay.begin(), contentPlay.end());
    patch32(body, contOffsBodyPos + 0, content0Off);
    patch32(body, contOffsBodyPos + 4, content1Off);
    patch32(body, contOffsBodyPos + 8, content2Off);

    std::vector<u8> block;
    putBytes(block, "pai1", 4);
    put32(block, static_cast<u32>(8 + body.size()));
    block.insert(block.end(), body.begin(), body.end());

    std::vector<u8> f;
    putBytes(f, "RLAN", 4);
    put16(f, 0xFEFF);
    put16(f, 0x000A);
    put32(f, static_cast<u32>(16 + block.size()));
    put16(f, 16);
    put16(f, 1);
    f.insert(f.end(), block.begin(), block.end());
    return f;
}

// --- RARC (una sola carpeta raíz, como layout_holder_test) ------------------

static u16 arcHash(const char* name) {
    u16 hash = 0;
    for (const char* p = name; *p; ++p) {
        hash = static_cast<u16>(static_cast<u16>(tolower(*p)) + hash * 3);
    }
    return hash;
}

static std::vector<u8> buildLayoutArcRarc(const std::vector<std::pair<std::string, std::vector<u8>>>& files) {
    std::vector<u8> strings;
    strings.push_back('.');
    strings.push_back(0);
    std::vector<u32> nameOffsets;
    for (const auto& e : files) {
        nameOffsets.push_back(static_cast<u32>(strings.size()));
        for (char c : e.first) strings.push_back(static_cast<u8>(c));
        strings.push_back(0);
    }
    const u32 nrDirs = 1;
    const u32 nrFiles = static_cast<u32>(files.size());
    const u32 dirOffset = 0x20;
    const u32 fileOffset = dirOffset + nrDirs * 0x10;
    const u32 stringTableOffset = fileOffset + nrFiles * 0x14;
    const u32 stringTableSize = static_cast<u32>(strings.size());
    const u32 headerSize = 0x20;
    const u32 tablesEnd = headerSize + 0x20 + stringTableOffset + stringTableSize;
    const u32 fileDataAbs = (tablesEnd + 0x1F) & ~0x1Fu;
    const u32 fileDataOffset = fileDataAbs - headerSize;

    std::vector<u32> dataOffsets;
    u32 cursor = 0;
    for (const auto& e : files) {
        dataOffsets.push_back(cursor);
        cursor = (cursor + static_cast<u32>(e.second.size()) + 0x1Fu) & ~0x1Fu;
    }
    const u32 totalDataSize = cursor;

    std::vector<u8> blob;
    putBytes(blob, "RARC", 4);
    const size_t fileSizePos = blob.size();
    put32(blob, 0);
    put32(blob, headerSize);
    put32(blob, fileDataOffset);
    put32(blob, totalDataSize);
    put32(blob, totalDataSize);
    put32(blob, 0);
    put32(blob, 0);
    put32(blob, nrDirs);
    put32(blob, dirOffset);
    put32(blob, nrFiles);
    put32(blob, fileOffset);
    put32(blob, stringTableSize);
    put32(blob, stringTableOffset);
    put16(blob, static_cast<u16>(nrFiles));
    put16(blob, 0);
    put32(blob, 0);
    put32(blob, 0x524F4F54);  // 'ROOT'
    put32(blob, 0);
    put16(blob, 0);
    put16(blob, static_cast<u16>(nrFiles));
    put32(blob, 0);
    for (u32 i = 0; i < nrFiles; i++) {
        put16(blob, static_cast<u16>(i));
        put16(blob, arcHash(files[i].first.c_str()));
        blob.push_back(0x11);
        blob.push_back(static_cast<u8>((nameOffsets[i] >> 16) & 0xFF));
        put16(blob, static_cast<u16>(nameOffsets[i] & 0xFFFF));
        put32(blob, dataOffsets[i]);
        put32(blob, static_cast<u32>(files[i].second.size()));
        put32(blob, 0);
    }
    blob.insert(blob.end(), strings.begin(), strings.end());
    while (blob.size() < fileDataAbs) blob.push_back(0);
    for (const auto& e : files) {
        blob.insert(blob.end(), e.second.begin(), e.second.end());
        while (blob.size() & 0x1F) blob.push_back(0);
    }
    patch32(blob, fileSizePos, static_cast<u32>(blob.size()));
    return blob;
}

int main(int argc, char** argv) {
    const char* outPath = argc > 1 ? argv[1] : "assets/LayoutData/WiiRemoteStrap.arc";

    // Texturas (RGBA8 fuente -> TPL GX tiled).
    std::vector<u8> texBg = solidRgba(8, 8, 208, 208, 208);
    // PicPlay: mitad superior ROJA, mitad inferior AZUL — asimétrica en Y a
    // propósito, para verificar la orientación con la sonda EFB del boot
    // (M9.5.3c: la pantalla del usuario salía boca abajo; el flip se arregló
    // en flushDraw y esta textura lo pinza end-to-end: arriba debe ser rojo).
    std::vector<u8> texPlay(static_cast<size_t>(568) * 392 * 4);
    for (u32 y = 0; y < 392; ++y) {
        for (u32 x = 0; x < 568; ++x) {
            u8* p = &texPlay[(static_cast<size_t>(y) * 568 + x) * 4];
            if (y < 196) {
                p[0] = 220; p[1] = 40; p[2] = 40; p[3] = 255;   // rojo arriba
            } else {
                p[0] = 50; p[1] = 80; p[2] = 230; p[3] = 255;   // azul abajo
            }
        }
    }
    std::vector<u8> texHook = solidRgba(360, 256, 40, 120, 220);
    std::vector<u8> texCur = solidRgba(40, 40, 250, 250, 250);
    // alt.tpl: VERDE solido 568x392 — el destino del swap RLTP del frame 100.
    std::vector<u8> texAlt = solidRgba(568, 392, 30, 180, 60);

    std::vector<u8> brlyt = makeStrapBrlyt();
    std::vector<u8> brlan = makeStrapBrlan();
    std::vector<u8> tplBg = makeTpl(8, 8, 0, texBg);
    std::vector<u8> tplPlay = makeTpl(568, 392, 4, texPlay);
    std::vector<u8> tplHook = makeTpl(360, 256, 4, texHook);
    std::vector<u8> tplCur = makeTpl(40, 40, 0, texCur);
    std::vector<u8> tplAlt = makeTpl(568, 392, 4, texAlt);

    std::vector<u8> rarc = buildLayoutArcRarc({
        {"wiiremotestrapreplace.brlyt", brlyt},
        {"strap.brlan", brlan},
        {"bg.tpl", tplBg},
        {"play.tpl", tplPlay},
        {"hook.tpl", tplHook},
        {"cursor.tpl", tplCur},
        {"alt.tpl", tplAlt},
    });

    FILE* f = std::fopen(outPath, "wb");
    if (!f) {
        std::fprintf(stderr, "ERROR: no se puede abrir %s\n", outPath);
        return 1;
    }
    std::fwrite(rarc.data(), 1, rarc.size(), f);
    std::fclose(f);
    std::printf("OK: %s (%u bytes; brlyt %u, brlan %u, tpl %u/%u/%u/%u)\n", outPath,
                static_cast<unsigned>(rarc.size()), static_cast<unsigned>(brlyt.size()),
                static_cast<unsigned>(brlan.size()), static_cast<unsigned>(tplBg.size()),
                static_cast<unsigned>(tplPlay.size()), static_cast<unsigned>(tplHook.size()),
                static_cast<unsigned>(tplCur.size()));
    return 0;
}
