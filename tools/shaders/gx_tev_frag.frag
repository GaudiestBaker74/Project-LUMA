#version 450

// GX TEV fragment shader (M5.4/M5.5): evaluates up to 16 chained TEV stages
// whose configuration arrives per-draw in a std140 UBO (set 1, dynamic), then
// applies the pixel-engine output stage (M5.5): 8-bit mask, alpha compare
// (GXSetAlphaCompare), the hardware "alpha == 1 does nothing in blending"
// quirk and fog (GXSetFog). The CPU reference of the exact same formulas
// lives in src/compat/gx/GXTev.cpp (evalTevChain / evalPixelEngine) — keep
// both in lockstep. Formula sources: Dolphin's PixelShaderGen
// (WriteTevRegular / WriteAlphaTest / WriteFog), see docs/gx.md §7.
//
// Per stage:
//   out = ((D + bias) << sl) ± (((((A<<8) + (B-A)·(C+(C>>7))) << sl) + r) >> 8)
//   out >>= sr, then clamp [0,255] or [-1024,1023]
// with sl/sr from scale (2/4 pre-shift, DIVIDE_2 post-shift), r = 128 (ADD) /
// 127 (SUB) unless DIVIDE_2. Comparison ops (op >= 8) select C on a compare of
// A vs B (mode (op>>1)&3, GT/EQ from op&1); the alpha combiner's R8/GR16/BGR24
// modes compare the COLOR combiner's A/B args (Dolphin semantics).
//
// Fog (Dolphin WriteFog, hardware-verified):
//   persp: ze = A·2^24 / (B_MAG − (Zs >> B_SHF));  ortho: ze = A·Zs/2^24
//   fog = clamp(ze − C, 0, 1);  fsel 4/5/6/7 = exp / exp² / rev-exp / rev-exp²
//   ifog = round(fog·256);  out = (out·(256−ifog) + fogColor·ifog) >> 8
// Zs (zCoord) is derived from gl_FragCoord.z assuming the GX inverted-depth
// convention (closer = larger Zs) and a [0,1] Vulkan depth range; the exact
// viewport/zscale handling for EFB copies lands in M5.7.

layout(location = 0) in vec4 vColor0;
layout(location = 1) in vec4 vColor1;
layout(location = 2) in vec2 vUV[8];

layout(set = 0, binding = 0) uniform sampler2D uTex[8];

layout(std140, set = 1, binding = 0) uniform TevUbo {
    ivec4 prev;        // initial TEVPREV (GXSetTevColor(TEVPREV))
    ivec4 tevReg[3];   // C0..C2 (signed 10-bit)
    ivec4 konst[4];    // K0..K3 (0..255)
    ivec4 stage[16][4];// [i][0]=colorEnv(a,b,c,d) [i][1]=alphaEnv(a,b,c,d)
                       // [i][2]=opParams(colorOpPack, alphaOpPack, orderPack,
                       //                 kselPack) [i][3]=tevind pack (M5.7b:
                       //                 indStage | fmt<<4 | bias<<8 | mtxSel<<12
                       //                 | wrapS<<16 | wrapT<<20 | addPrev<<24)
    ivec4 header;      // x = num stages, y = num texgens, z = num ind stages
    ivec4 swapTables[4]; // table -> r,g,b,a channel indices
    // --- M5.5 pixel-engine extras ---
    ivec4 fogColor;    // xyz = fog color RGB (0..255)
    ivec4 fogReg;      // x = B_MAG (u0.24), y = B_SHF, z = fsel, w = proj
    vec4  fogAB;       // x = A (float), y = C (float)
    ivec4 alphaCmp;    // x = comp0(3) | ref0(8) | logic(2) | comp1(3) | ref1(8)
    // --- M5.7b indirect stages ---
    ivec4 indParams[4];// x = baseCoord | indMap<<8 | scaleS<<16 | scaleT<<20
    vec4  indMtx[6];   // 2 rows per indirect matrix: xyz = row (m0,m1,m2),
                       // w = 128 / 2^scaleExp (texel-space calibration)
    vec4  texDims[8];  // xy = texel size (w,h) of the stage's direct map (for
                       // normalizing the indirect offset back to UV space)
} tev;

layout(location = 0) out vec4 outColor;

// K constant select -> component value (GX SDK semantics; Dolphin treats the
// 8..11 selects as 0 — documented deviation in gx.md §7).
int konstChannel(int sel, int chan) {
    if (sel <= 7) {
        const int fracs[8] = int[8](255, 223, 191, 159, 128, 96, 64, 32);
        return fracs[sel];
    }
    // sel 8..11 (0x08-0x0B): invalid selects, treated as 0 (Dolphin). No GX
    // enum produces them; kept for lockstep with the CPU reference.
    if (sel >= 12 && sel <= 15) return tev.konst[sel - 12][chan];
    if (sel >= 16 && sel <= 19) return tev.konst[sel - 16][0];
    if (sel >= 20 && sel <= 23) return tev.konst[sel - 20][1];
    if (sel >= 24 && sel <= 27) return tev.konst[sel - 24][2];
    if (sel >= 28 && sel <= 31) return tev.konst[sel - 28][3];
    return 0;
}
int konstAlpha(int sel) {
    return (sel >= 12 && sel <= 15) ? 0 : konstChannel(sel, 0);
}

// Color combiner arg fetch -> rgb. arg = GXTevColorArg (0..15).
ivec3 fetchColor(int arg, ivec4 prev, ivec4 c0, ivec4 c1, ivec4 c2, ivec4 textemp,
                 ivec4 rastemp, ivec4 konsttemp) {
    switch (arg) {
        case 0:  return prev.rgb;    // CPREV
        case 1:  return prev.aaa;    // APREV
        case 2:  return c0.rgb;      // C0
        case 3:  return c0.aaa;      // A0
        case 4:  return c1.rgb;      // C1
        case 5:  return c1.aaa;      // A1
        case 6:  return c2.rgb;      // C2
        case 7:  return c2.aaa;      // A2
        case 8:  return textemp.rgb; // TEXC
        case 9:  return textemp.aaa; // TEXA
        case 10: return rastemp.rgb; // RASC
        case 11: return rastemp.aaa; // RASA
        case 12: return ivec3(255);  // ONE
        case 13: return ivec3(128);  // HALF
        case 14: return konsttemp.rgb; // KONST
        default: return ivec3(0);    // ZERO
    }
}
// Alpha combiner arg fetch -> scalar. arg = GXTevAlphaArg (0..7).
int fetchAlpha(int arg, ivec4 prev, ivec4 c0, ivec4 c1, ivec4 c2, ivec4 textemp,
               ivec4 rastemp, ivec4 konsttemp) {
    switch (arg) {
        case 0: return prev.a;      // APREV
        case 1: return c0.a;        // A0
        case 2: return c1.a;        // A1
        case 3: return c2.a;        // A2
        case 4: return textemp.a;   // TEXA
        case 5: return rastemp.a;   // RASA
        case 6: return konsttemp.a; // KONST
        default: return 0;          // ZERO
    }
}

// Regular combine (Dolphin formula; scale: 0=1, 1=2, 2=4, 3=DIVIDE_2).
int tevCombine(int a, int b, int c, int d, int op, int bias, int scale) {
    const int sl = (scale == 1) ? 1 : (scale == 2) ? 2 : 0;
    const int biasV = (bias == 1) ? 128 : (bias == 2) ? -128 : 0;
    int lerp = (a << 8) + (b - a) * (c + (c >> 7));  // c>>7: arithmetic shift
    if (scale != 3) {
        lerp = (lerp << sl) + ((op == 0) ? 128 : 127);
    }
    lerp >>= 8;
    const int db = (d + biasV) << sl;
    int result = (op == 0) ? (db + lerp) : (db - lerp);
    if (scale == 3) {
        result >>= 1;
    }
    return result;
}

int clampReg(int v, bool tevClamp) {
    return tevClamp ? clamp(v, 0, 255) : clamp(v, -1024, 1023);
}

// GXCompare on (alpha, ref): NEVER=0, LESS=1, EQUAL=2, LEQUAL=3, GREATER=4,
// NEQUAL=5, GEQUAL=6, ALWAYS=7.
bool alphaCompare(int comp, int alpha, int ref) {
    switch (comp) {
        case 0: return false;
        case 1: return alpha < ref;
        case 2: return alpha == ref;
        case 3: return alpha <= ref;
        case 4: return alpha > ref;
        case 5: return alpha != ref;
        case 6: return alpha >= ref;
        default: return true;
    }
}

void main() {
    ivec4 prev = tev.prev;
    ivec4 c0 = tev.tevReg[0];
    ivec4 c1 = tev.tevReg[1];
    ivec4 c2 = tev.tevReg[2];
    ivec4 ras0 = ivec4(round(vColor0 * 255.0));
    ivec4 ras1 = ivec4(round(vColor1 * 255.0));

    // --- M5.7b: indirect texture stages --------------------------------------
    // Sample the indirect maps once per fragment (per ind stage): the raw
    // texel (0..1) is quantized per TEV stage in the warp below. The sample
    // coordinate is the stage's base texcoord scaled down by IndTexCoordScale
    // (Dolphin: fixpoint_uv >> indtexscale, in UV space: / (1<<scale)).
    const int numInd = tev.header.z;
    vec3 iind[4] = vec3[4](vec3(0.0), vec3(0.0), vec3(0.0), vec3(0.0));
    for (int i = 0; i < 4; ++i) {
        if (i < numInd) {
            const int p = tev.indParams[i].x;
            const int coord = p & 0xFF;
            const int map = (p >> 8) & 0xFF;
            const int scaleS = min((p >> 16) & 0xFF, 8);
            const int scaleT = min((p >> 24) & 0xFF, 8);
            const vec2 indUv = vUV[coord] / vec2(float(1 << scaleS), float(1 << scaleT));
            if (map == 0) {
                iind[i] = texture(uTex[0], indUv).rgb;
            } else if (map == 1) {
                iind[i] = texture(uTex[1], indUv).rgb;
            } else if (map == 2) {
                iind[i] = texture(uTex[2], indUv).rgb;
            } else if (map == 3) {
                iind[i] = texture(uTex[3], indUv).rgb;
            } else if (map == 4) {
                iind[i] = texture(uTex[4], indUv).rgb;
            } else if (map == 5) {
                iind[i] = texture(uTex[5], indUv).rgb;
            } else if (map == 6) {
                iind[i] = texture(uTex[6], indUv).rgb;
            } else {
                iind[i] = texture(uTex[7], indUv).rgb;
            }
        }
    }
    // Warped coordinate of the previous TEV stage (GX fb_addprev).
    vec2 prevIndCoord = vec2(0.0);

    const int numStages = tev.header.x;
    for (int i = 0; i < numStages; ++i) {
        const ivec4 cenv = tev.stage[i][0];
        const ivec4 aenv = tev.stage[i][1];
        const ivec4 opPack = tev.stage[i][2];
        const int colorOpPack = opPack.x;
        const int alphaOpPack = opPack.y;
        const int orderPack = opPack.z;
        const int kselPack = opPack.w;

        // --- stage inputs ---------------------------------------------------
        const int texmap = orderPack & 0xFF;
        int texcoord = (orderPack >> 8) & 0xFF;
        const int colorChan = (orderPack >> 16) & 0xFF;
        const bool texEnable = ((orderPack >> 24) & 1) != 0;
        // Quirk (Dolphin): stage texcoord >= num texgens falls back to coord 0.
        if (texcoord >= tev.header.y) {
            texcoord = 0;
        }

        // --- M5.7b: indirect warp on the stage texcoord ----------------------
        // tevind pack (stage[i][3].x): indStage | fmt<<4 | bias<<8 | mtxSel<<12
        // | wrapS<<16 | wrapT<<20 | addPrev<<24 (see packIndTevUbo).
        const int tevind = tev.stage[i][3].x;
        const int indStage = tevind & 0xF;
        const int indFmt = (tevind >> 4) & 0xF;
        const int indBias = (tevind >> 8) & 0xF;
        const int indMtxSel = (tevind >> 12) & 0xF;
        const int indWrapS = (tevind >> 16) & 0x7;
        const int indWrapT = (tevind >> 20) & 0x7;
        const bool indAddPrev = ((tevind >> 24) & 1) != 0;
        vec2 sampleUv = vUV[texcoord];
        if (indStage < numInd && indMtxSel >= 1 && indMtxSel <= 3) {
            // Quantize the raw indirect texel to the format's significant bits
            // (Dolphin iindtevcrd: ITF_8 keeps 8 bits, ITF_5/4/3 shift left the
            // top 5/4/3 bits). Then center the components covered by the bias
            // (GX_ITB_S/T/U bits): 0..1 -> -1..1.
            vec3 comp = iind[indStage];
            if (indFmt == 1) comp = floor(comp * 255.0 / 8.0) / 31.0;
            else if (indFmt == 2) comp = floor(comp * 255.0 / 16.0) / 15.0;
            else if (indFmt == 3) comp = floor(comp * 255.0 / 32.0) / 7.0;
            if ((indBias & 1) != 0) comp.x = comp.x * 2.0 - 1.0;  // S
            if ((indBias & 2) != 0) comp.y = comp.y * 2.0 - 1.0;  // T
            if ((indBias & 4) != 0) comp.z = comp.z * 2.0 - 1.0;  // U

            // 2x3 matrix (GX_ITM_0..2 = index 0..2): offset in texels =
            // (M·[s,t,1]) * 128/2^scaleExp (indMtx[..].w), normalized to UV by
            // the direct map's texel size.
            const vec4 r0 = tev.indMtx[(indMtxSel - 1) * 2 + 0];
            const vec4 r1 = tev.indMtx[(indMtxSel - 1) * 2 + 1];
            vec2 offsetUv = vec2(dot(r0.xyz, vec3(comp.x, comp.y, 1.0)),
                                 dot(r1.xyz, vec3(comp.x, comp.y, 1.0))) * r0.w;
            const vec2 texDim = tev.texDims[texmap].xy;
            if (texDim.x > 0.0 && texDim.y > 0.0) {
                offsetUv /= texDim;
            }

            // Wrap the base texcoord (GX_ITW_*): OFF = as-is, 0 = zero,
            // 256..16 = repeat every N texels (mod, approximated in UV).
            vec2 wrapped = vUV[texcoord];
            if (indWrapS == 6) {
                wrapped.x = 0.0;
            } else if (indWrapS >= 1 && indWrapS <= 5) {
                const float n = float(1 << (9 - indWrapS));  // 256,128,...,16
                if (texDim.x > 0.0) {
                    wrapped.x = fract(vUV[texcoord].x * texDim.x / n) * (n / texDim.x);
                }
            }
            if (indWrapT == 6) {
                wrapped.y = 0.0;
            } else if (indWrapT >= 1 && indWrapT <= 5) {
                const float n = float(1 << (9 - indWrapT));
                if (texDim.y > 0.0) {
                    wrapped.y = fract(vUV[texcoord].y * texDim.y / n) * (n / texDim.y);
                }
            }
            sampleUv = wrapped + offsetUv;
            if (indAddPrev) {
                sampleUv += prevIndCoord;
            }
            prevIndCoord = sampleUv;
        }

        // Raster color (col0/col1 by channel; bump/zero otherwise — M5.7).
        ivec4 ras = ivec4(0);
        if (colorChan == 0) {
            ras = ras0;
        } else if (colorChan == 1) {
            ras = ras1;
        }

        // Texture: enabled + texgens>0 -> sample; no texgens -> black; else
        // white (Dolphin semantics).
        ivec4 texel = ivec4(255);
        if (texEnable && tev.header.y > 0) {
            // Static-index branch over the 8 maps: dynamic sampler indexing
            // needs shaderSampledImageArrayDynamicIndexing, which the renderer
            // does not enable. texmap comes from the UBO, so the branch is
            // uniform across the draw and every taken path samples a constant
            // index.
            if (texmap == 0) {
                texel = ivec4(round(texture(uTex[0], sampleUv) * 255.0));
            } else if (texmap == 1) {
                texel = ivec4(round(texture(uTex[1], sampleUv) * 255.0));
            } else if (texmap == 2) {
                texel = ivec4(round(texture(uTex[2], sampleUv) * 255.0));
            } else if (texmap == 3) {
                texel = ivec4(round(texture(uTex[3], sampleUv) * 255.0));
            } else if (texmap == 4) {
                texel = ivec4(round(texture(uTex[4], sampleUv) * 255.0));
            } else if (texmap == 5) {
                texel = ivec4(round(texture(uTex[5], sampleUv) * 255.0));
            } else if (texmap == 6) {
                texel = ivec4(round(texture(uTex[6], sampleUv) * 255.0));
            } else {
                texel = ivec4(round(texture(uTex[7], sampleUv) * 255.0));
            }
        } else if (tev.header.y == 0) {
            texel = ivec4(0);
        }

        // Swap tables (rasSel/texSel from the alpha op pack).
        const ivec4 rasSwap = tev.swapTables[(alphaOpPack >> 12) & 3];
        const ivec4 texSwap = tev.swapTables[(alphaOpPack >> 14) & 3];
        ivec4 rastemp = ivec4(ras[rasSwap.x], ras[rasSwap.y], ras[rasSwap.z], ras[rasSwap.w]);
        ivec4 textemp = ivec4(texel[texSwap.x], texel[texSwap.y], texel[texSwap.z],
                              texel[texSwap.w]);

        ivec4 konsttemp = ivec4(konstChannel(kselPack & 0xFF, 0),
                                konstChannel(kselPack & 0xFF, 1),
                                konstChannel(kselPack & 0xFF, 2),
                                konstAlpha((kselPack >> 8) & 0xFF));

        // --- color combine --------------------------------------------------
        const int cOp = colorOpPack & 0x1F;
        const int cBias = (colorOpPack >> 5) & 3;
        const int cScale = (colorOpPack >> 7) & 3;
        const bool cClamp = ((colorOpPack >> 9) & 1) != 0;
        const int cDest = (colorOpPack >> 10) & 3;

        const ivec3 cA = fetchColor(cenv.x, prev, c0, c1, c2, textemp, rastemp, konsttemp);
        const ivec3 cB = fetchColor(cenv.y, prev, c0, c1, c2, textemp, rastemp, konsttemp);
        const ivec3 cC = fetchColor(cenv.z, prev, c0, c1, c2, textemp, rastemp, konsttemp);
        const ivec3 cD = fetchColor(cenv.w, prev, c0, c1, c2, textemp, rastemp, konsttemp);

        ivec3 coutv;
        if (cOp >= 8) {
            // Comparison ops (GX_TEV_COMP_*): mode (op>>1)&3, GT if op even.
            const int cmpMode = (cOp >> 1) & 3;
            const bool eq = (cOp & 1) == 1;
            ivec3 csel = ivec3(0);
            if (cmpMode == 0) {            // R8
                const bool hit = eq ? (cA.r == cB.r) : (cA.r > cB.r);
                if (hit) csel = cC;
            } else if (cmpMode == 1) {     // GR16 (weights 1,256,0)
                const int da = cA.r + 256 * cA.g;
                const int db = cB.r + 256 * cB.g;
                const bool hit = eq ? (da == db) : (da > db);
                if (hit) csel = cC;
            } else if (cmpMode == 2) {     // BGR24 (weights 1,256,65536)
                const int da = cA.r + 256 * cA.g + 65536 * cA.b;
                const int db = cB.r + 256 * cB.g + 65536 * cB.b;
                const bool hit = eq ? (da == db) : (da > db);
                if (hit) csel = cC;
            } else {                       // RGB8 (per channel)
                if (eq) {
                    csel = (ivec3(1) - sign(abs(cA - cB))) * cC;
                } else {
                    csel = max(sign(cA - cB), ivec3(0)) * cC;
                }
            }
            coutv = cD + csel;
        } else {
            coutv = ivec3(tevCombine(cA.x, cB.x, cC.x, cD.x, cOp, cBias, cScale),
                          tevCombine(cA.y, cB.y, cC.y, cD.y, cOp, cBias, cScale),
                          tevCombine(cA.z, cB.z, cC.z, cD.z, cOp, cBias, cScale));
        }
        coutv = ivec3(clampReg(coutv.x, cClamp), clampReg(coutv.y, cClamp),
                      clampReg(coutv.z, cClamp));

        if (cDest == 0) {
            prev.rgb = coutv;
        } else if (cDest == 1) {
            c0.rgb = coutv;
        } else if (cDest == 2) {
            c1.rgb = coutv;
        } else {
            c2.rgb = coutv;
        }

        // --- alpha combine --------------------------------------------------
        const int aOp = alphaOpPack & 0x1F;
        const int aBias = (alphaOpPack >> 5) & 3;
        const int aScale = (alphaOpPack >> 7) & 3;
        const bool aClamp = ((alphaOpPack >> 9) & 1) != 0;
        const int aDest = (alphaOpPack >> 10) & 3;

        const int aA = fetchAlpha(aenv.x, prev, c0, c1, c2, textemp, rastemp, konsttemp);
        const int aB = fetchAlpha(aenv.y, prev, c0, c1, c2, textemp, rastemp, konsttemp);
        const int aC = fetchAlpha(aenv.z, prev, c0, c1, c2, textemp, rastemp, konsttemp);
        const int aD = fetchAlpha(aenv.w, prev, c0, c1, c2, textemp, rastemp, konsttemp);

        int aout;
        if (aOp >= 8) {
            const int cmpMode = (aOp >> 1) & 3;
            const bool eq = (aOp & 1) == 1;
            int asel = 0;
            if (cmpMode == 0) {            // R8 — compares the COLOR combiner's A/B
                const bool hit = eq ? (cA.r == cB.r) : (cA.r > cB.r);
                if (hit) asel = aC;
            } else if (cmpMode == 1) {     // GR16
                const int da = cA.r + 256 * cA.g;
                const int db = cB.r + 256 * cB.g;
                const bool hit = eq ? (da == db) : (da > db);
                if (hit) asel = aC;
            } else if (cmpMode == 2) {     // BGR24
                const int da = cA.r + 256 * cA.g + 65536 * cA.b;
                const int db = cB.r + 256 * cB.g + 65536 * cB.b;
                const bool hit = eq ? (da == db) : (da > db);
                if (hit) asel = aC;
            } else {                       // A8 — the alpha combiner's own A/B
                const bool hit = eq ? (aA == aB) : (aA > aB);
                if (hit) asel = aC;
            }
            aout = aD + asel;
        } else {
            aout = tevCombine(aA, aB, aC, aD, aOp, aBias, aScale);
        }
        aout = clampReg(aout, aClamp);

        if (aDest == 0) {
            prev.a = aout;
        } else if (aDest == 1) {
            c0.a = aout;
        } else if (aDest == 2) {
            c1.a = aout;
        } else {
            c2.a = aout;
        }
    }

    // The last stage's result goes to the screen regardless of its dest
    // register (Dolphin quirk).
    const int lastC = (numStages > 0) ? numStages - 1 : 0;
    if (numStages > 0) {
        const int lastColorDest = (tev.stage[lastC][2].x >> 10) & 3;
        const int lastAlphaDest = (tev.stage[lastC][2].y >> 10) & 3;
        if (lastColorDest == 1) {
            prev.rgb = c0.rgb;
        } else if (lastColorDest == 2) {
            prev.rgb = c1.rgb;
        } else if (lastColorDest == 3) {
            prev.rgb = c2.rgb;
        }
        if (lastAlphaDest == 1) {
            prev.a = c0.a;
        } else if (lastAlphaDest == 2) {
            prev.a = c1.a;
        } else if (lastAlphaDest == 3) {
            prev.a = c2.a;
        }
    }

    // --- pixel-engine output stage (M5.5) ------------------------------------
    // 8-bit output: the TEV result is cut to 8 bits per component (Dolphin:
    // "prev = frag_output.main & 255").
    prev &= 255;

    // Alpha compare (GXSetAlphaCompare): pass = comp0(alpha) LOGIC comp1(alpha);
    // discard on failure. GXInit's ALWAYS/0/AND/ALWAYS/0 always passes.
    {
        const int pack = tev.alphaCmp.x;
        const int comp0 = pack & 7;
        const int ref0 = (pack >> 3) & 0xFF;
        const int logic = (pack >> 11) & 3;
        const int comp1 = (pack >> 13) & 7;
        const int ref1 = (pack >> 16) & 0xFF;
        const bool pass0 = alphaCompare(comp0, prev.a, ref0);
        const bool pass1 = alphaCompare(comp1, prev.a, ref1);
        bool pass;
        if (logic == 0) {
            pass = pass0 && pass1;   // GX_AOP_AND
        } else if (logic == 1) {
            pass = pass0 || pass1;   // GX_AOP_OR
        } else if (logic == 2) {
            pass = pass0 != pass1;   // GX_AOP_XOR
        } else {
            pass = pass0 == pass1;   // GX_AOP_XNOR
        }
        if (!pass) {
            discard;
        }
    }

    // Hardware quirk (Dolphin): "an alpha of 1 can pass an alpha test, but
    // doesn't do anything in blending" — zero the alpha so fixed-function
    // blending leaves the framebuffer unchanged.
    if (prev.a == 1) {
        prev.a = 0;
    }

    // Fog (GXSetFog). fsel 0 = off.
    if (tev.fogReg.z != 0) {
        // zCoord: 24-bit GX depth derived from the Vulkan [0,1] depth value
        // (GX convention: closer fragments have a larger Zs).
        const int zCoord = clamp(int((1.0 - gl_FragCoord.z) * 16777216.0), 0, 0xFFFFFF);
        float ze;
        if (tev.fogReg.w == 0) {
            // perspective: ze = A·2^24 / (B_MAG − (Zs >> B_SHF))
            const int bShifted = zCoord >> tev.fogReg.y;
            ze = (tev.fogAB.x * 16777216.0) / float(tev.fogReg.x - bShifted);
        } else {
            // orthographic: ze = A·Zs / 2^24
            ze = tev.fogAB.x * float(zCoord) / 16777216.0;
        }
        float fog = clamp(ze - tev.fogAB.y, 0.0, 1.0);
        if (tev.fogReg.z == 4) {
            fog = 1.0 - exp2(-8.0 * fog);                    // EXP
        } else if (tev.fogReg.z == 5) {
            fog = 1.0 - exp2(-8.0 * fog * fog);              // EXP2
        } else if (tev.fogReg.z == 6) {
            fog = exp2(-8.0 * (1.0 - fog));                  // REVEXP
        } else if (tev.fogReg.z == 7) {
            fog = 1.0 - fog;
            fog = exp2(-8.0 * fog * fog);                    // REVEXP2
        }
        const int ifog = int(round(fog * 256.0));
        prev.rgb = (prev.rgb * (256 - ifog) + tev.fogColor.rgb * ifog) >> 8;
    }

    outColor = vec4(prev) * (1.0 / 255.0);
}
