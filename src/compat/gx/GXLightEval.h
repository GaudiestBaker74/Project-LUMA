#pragma once
// =============================================================================
// Hot-path GX channel lighting. Included from GXLightInternal.h.
//
// The Wii XF unit lights every vertex in hardware. The host evaluates the same
// formula on the CPU (Dolphin LightingShaderGen). File Select submits ~44k lit
// vertices per frame; a cross-TU call plus a 24-slot zero-init per vertex is
// what drops that screen to ~10 FPS. This header is the same formula, arranged
// so the vertex loop can inline it:
//
//   * geometry is computed once per light (not once per channel slot)
//   * the LightGeom cache is NOT value-initialized (a bitmask records hits)
//   * when every enabled slot shares attenuation, diffuse and light mask — the
//     File Select planets do — a template drops the per-vertex switches
//
// Results match computeChannelLighting / applyChannelLighting (the tests call
// those, and they forward here). No fast-math: sqrt and the round-half-away
// quantize stay IEEE, so colors do not shift.
// =============================================================================

#include <cmath>
#include <cstring>

namespace Platform::CompatGx {

inline int gxFastRound(float x) {
    // Bit-identical to std::round for |x| < 2^23 (color and light products).
    return static_cast<int>(x + (x >= 0.0f ? 0.5f : -0.5f));
}

inline int gxRound255(float v) { return gxFastRound(v * 255.0f); }

inline int gxMul255(int mat, int lacc) { return (mat * (lacc + (lacc >> 7))) >> 8; }

inline int gxClamp255(int v) {
    if (v < 0) {
        return 0;
    }
    if (v > 255) {
        return 255;
    }
    return v;
}

inline float gxMax0(float v) { return v > 0.0f ? v : 0.0f; }

// attnFn: 0 = SPEC, 1 = SPOT, anything else = NONE. Copied from
// computeChannelLighting so the two paths cannot drift.
template <int AttnFn>
inline void gxFillLightGeom(const LightParams& L, const float posView[3], const float nrmView[3],
                            float ldir[3], float& attn, float& ndl) {
    const float d0 = L.pos[0] - posView[0];
    const float d1 = L.pos[1] - posView[1];
    const float d2 = L.pos[2] - posView[2];
    if constexpr (AttnFn == 0) {
        const float lenSq = d0 * d0 + d1 * d1 + d2 * d2;
        if (lenSq == 0.0f) {
            ldir[0] = nrmView[0];
            ldir[1] = nrmView[1];
            ldir[2] = nrmView[2];
        } else {
            const float inv = 1.0f / std::sqrt(lenSq);
            ldir[0] = d0 * inv;
            ldir[1] = d1 * inv;
            ldir[2] = d2 * inv;
        }
        ndl = nrmView[0] * ldir[0] + nrmView[1] * ldir[1] + nrmView[2] * ldir[2];
        const float attn0 = (ndl >= 0.0f) ? gxMax0(nrmView[0] * L.dir[0] + nrmView[1] * L.dir[1] +
                                                   nrmView[2] * L.dir[2])
                                           : 0.0f;
        const float cosAttn = gxMax0(L.a[0] + L.a[1] * attn0 + L.a[2] * attn0 * attn0);
        const float distAttn = L.k[0] + L.k[1] * attn0 + L.k[2] * attn0 * attn0;
        attn = (distAttn != 0.0f) ? cosAttn / distAttn : 0.0f;
    } else if constexpr (AttnFn == 1) {
        const float dist2 = d0 * d0 + d1 * d1 + d2 * d2;
        const float dist = std::sqrt(dist2);
        if (dist != 0.0f) {
            ldir[0] = d0 / dist;
            ldir[1] = d1 / dist;
            ldir[2] = d2 / dist;
        } else {
            ldir[0] = nrmView[0];
            ldir[1] = nrmView[1];
            ldir[2] = nrmView[2];
        }
        ndl = nrmView[0] * ldir[0] + nrmView[1] * ldir[1] + nrmView[2] * ldir[2];
        const float cosA = gxMax0(ldir[0] * L.dir[0] + ldir[1] * L.dir[1] + ldir[2] * L.dir[2]);
        const float cosAttn = gxMax0(L.a[0] + L.a[1] * cosA + L.a[2] * cosA * cosA);
        const float distAttn = L.k[0] + L.k[1] * dist + L.k[2] * dist2;
        attn = (distAttn != 0.0f) ? cosAttn / distAttn : 0.0f;
    } else {
        const float lenSq = d0 * d0 + d1 * d1 + d2 * d2;
        if (lenSq == 0.0f) {
            ldir[0] = nrmView[0];
            ldir[1] = nrmView[1];
            ldir[2] = nrmView[2];
        } else {
            const float inv = 1.0f / std::sqrt(lenSq);
            ldir[0] = d0 * inv;
            ldir[1] = d1 * inv;
            ldir[2] = d2 * inv;
        }
        ndl = nrmView[0] * ldir[0] + nrmView[1] * ldir[1] + nrmView[2] * ldir[2];
        attn = 1.0f;
    }
}

template <int DiffFn>
inline float gxDiffuse(float attn, float ndl) {
    if constexpr (DiffFn == 1) {
        return attn * ndl;
    } else if constexpr (DiffFn == 2) {
        return attn * gxMax0(ndl);
    } else {
        return attn;
    }
}

// True when every ENABLED slot shares attenuation, diffuse and light mask, so
// the per-vertex loop can drop its switches. Disabled slots are ignored here
// (they still contribute material*255 in the evaluator). Returns false when
// nothing is lit — the general path is already cheap in that case.
inline bool uniformLighting(const ChanLightState chan[4], int* attn, int* diff,
                            unsigned* mask) {
    bool any = false;
    int a = 0;
    int d = 0;
    unsigned m = 0;
    for (int j = 0; j < 4; ++j) {
        if (chan[j].enable == 0) {
            continue;
        }
        if (!any) {
            any = true;
            a = chan[j].attnFn;
            d = chan[j].diffFn;
            m = chan[j].lightMask;
        } else if (chan[j].attnFn != a || chan[j].diffFn != d || chan[j].lightMask != m) {
            return false;
        }
    }
    if (!any) {
        return false;
    }
    *attn = a;
    *diff = d;
    *mask = m;
    return true;
}

// Shared-mask evaluator. AttnFn/DiffFn are the values every enabled slot uses.
// Disabled slots take the hardware "channel off" path (accumulator 255).
template <int AttnFn, int DiffFn>
inline void evalUniformLighting(const ChanLightState chan[4], const std::uint8_t amb[2][4],
                                const std::uint8_t mat[2][4], const LightParams* lights,
                                unsigned lightMask, const float posView[3], const float nrmView[3],
                                const float clr0[4], const float clr1[4], float out0[4],
                                float out1[4]) {
    float ldir[8][3];
    float attn[8];
    float ndl[8];
    unsigned geomMask = 0;
    // Walk lights in index order so the rounded accumulation matches the
    // general evaluator (addition of rounded terms is not associative).
    for (int li = 0; li < 8; ++li) {
        if ((lightMask & (1u << li)) == 0) {
            continue;
        }
        gxFillLightGeom<AttnFn>(lights[li], posView, nrmView, ldir[li], attn[li], ndl[li]);
        geomMask |= 1u << li;
    }

    float* outPair[2] = {out0, out1};
    const float* basePair[2] = {clr0, clr1};
    for (int j = 0; j < 4; ++j) {
        const bool isColor = (j == 0 || j == 1);
        const int pair = j & 1;
        const ChanLightState& c = chan[j];
        const float* base = basePair[pair];

        int matV[4];
        if (c.matSrc == 1) {
            for (int i = 0; i < 4; ++i) {
                matV[i] = gxRound255(base[i]);
            }
        } else {
            for (int i = 0; i < 4; ++i) {
                matV[i] = mat[pair][i];
            }
        }

        int lacc[4];
        if (c.enable != 0) {
            if (c.ambSrc == 1) {
                for (int i = 0; i < 4; ++i) {
                    lacc[i] = gxRound255(base[i]);
                }
            } else {
                for (int i = 0; i < 4; ++i) {
                    lacc[i] = amb[pair][i];
                }
            }
            for (int li = 0; li < 8; ++li) {
                if ((geomMask & (1u << li)) == 0) {
                    continue;
                }
                const float diffuse = gxDiffuse<DiffFn>(attn[li], ndl[li]);
                const LightParams& L = lights[li];
                if (isColor) {
                    lacc[0] += gxFastRound(diffuse * static_cast<float>(L.color[0]));
                    lacc[1] += gxFastRound(diffuse * static_cast<float>(L.color[1]));
                    lacc[2] += gxFastRound(diffuse * static_cast<float>(L.color[2]));
                } else {
                    lacc[3] += gxFastRound(diffuse * static_cast<float>(L.color[3]));
                }
            }
        } else {
            lacc[0] = lacc[1] = lacc[2] = lacc[3] = 255;
        }

        for (int i = 0; i < 4; ++i) {
            lacc[i] = gxMul255(matV[i], gxClamp255(lacc[i]));
        }
        if (isColor) {
            outPair[pair][0] = lacc[0] / 255.0f;
            outPair[pair][1] = lacc[1] / 255.0f;
            outPair[pair][2] = lacc[2] / 255.0f;
        } else {
            outPair[pair][3] = lacc[3] / 255.0f;
        }
    }
}

// General evaluator: mixed per-slot attenuation. Same expressions as the
// historical computeChannelLighting, without the 24-struct zero-init.
inline void evaluateChannelLighting(const ChanLightState chan[4], const std::uint8_t amb[2][4],
                                    const std::uint8_t mat[2][4], const LightParams* lights,
                                    const float posView[3], const float nrmView[3],
                                    const float clr0[4], const float clr1[4], float out0[4],
                                    float out1[4]) {
    struct Geom {
        float ldir[3];
        float attn;
        float ndl;
    };
    Geom geom[3][8];
    unsigned valid[3] = {0, 0, 0};

    float* outPair[2] = {out0, out1};
    const float* basePair[2] = {clr0, clr1};
    for (int j = 0; j < 4; ++j) {
        const bool isColor = (j == 0 || j == 1);
        const int pair = j & 1;
        const float* base = basePair[pair];
        const ChanLightState& c = chan[j];

        int matV[4];
        if (c.matSrc == 1) {
            for (int i = 0; i < 4; ++i) {
                matV[i] = gxRound255(base[i]);
            }
        } else {
            for (int i = 0; i < 4; ++i) {
                matV[i] = mat[pair][i];
            }
        }

        int lacc[4];
        if (c.enable != 0) {
            if (c.ambSrc == 1) {
                for (int i = 0; i < 4; ++i) {
                    lacc[i] = gxRound255(base[i]);
                }
            } else {
                for (int i = 0; i < 4; ++i) {
                    lacc[i] = amb[pair][i];
                }
            }
            for (int li = 0; li < 8; ++li) {
                if ((c.lightMask & (1u << li)) == 0) {
                    continue;
                }
                const int fn = (c.attnFn == 0 || c.attnFn == 1) ? c.attnFn : 2;
                Geom& g = geom[fn][li];
                if ((valid[fn] & (1u << li)) == 0) {
                    valid[fn] |= 1u << li;
                    if (fn == 0) {
                        gxFillLightGeom<0>(lights[li], posView, nrmView, g.ldir, g.attn, g.ndl);
                    } else if (fn == 1) {
                        gxFillLightGeom<1>(lights[li], posView, nrmView, g.ldir, g.attn, g.ndl);
                    } else {
                        gxFillLightGeom<2>(lights[li], posView, nrmView, g.ldir, g.attn, g.ndl);
                    }
                }
                float diffuse;
                if (c.diffFn == 1) {
                    diffuse = g.attn * g.ndl;
                } else if (c.diffFn == 2) {
                    diffuse = g.attn * gxMax0(g.ndl);
                } else {
                    diffuse = g.attn;
                }
                const LightParams& L = lights[li];
                if (isColor) {
                    lacc[0] += gxFastRound(diffuse * static_cast<float>(L.color[0]));
                    lacc[1] += gxFastRound(diffuse * static_cast<float>(L.color[1]));
                    lacc[2] += gxFastRound(diffuse * static_cast<float>(L.color[2]));
                } else {
                    lacc[3] += gxFastRound(diffuse * static_cast<float>(L.color[3]));
                }
            }
        } else {
            lacc[0] = lacc[1] = lacc[2] = lacc[3] = 255;
        }
        for (int i = 0; i < 4; ++i) {
            lacc[i] = gxMul255(matV[i], gxClamp255(lacc[i]));
        }
        if (isColor) {
            outPair[pair][0] = lacc[0] / 255.0f;
            outPair[pair][1] = lacc[1] / 255.0f;
            outPair[pair][2] = lacc[2] / 255.0f;
        } else {
            outPair[pair][3] = lacc[3] / 255.0f;
        }
    }
}

// Dispatch used by the vertex loop. The switch is on per-draw constants; each
// arm inlines a switch-free evaluator. Mixed slot setups take the general path.
inline void evalLightingHot(const ChanLightState chan[4], const std::uint8_t amb[2][4],
                            const std::uint8_t mat[2][4], const LightParams* lights, int uniAttn,
                            int uniDiff, unsigned uniMask, bool uniform, const float posView[3],
                            const float nrmView[3], const float clr0[4], const float clr1[4],
                            float out0[4], float out1[4]) {
    if (!uniform) {
        evaluateChannelLighting(chan, amb, mat, lights, posView, nrmView, clr0, clr1, out0, out1);
        return;
    }
    // attn 0/1/2 × diff 0/1/2. SPEC (attn 0) is only ever paired with diff NONE
    // by the SDK, but a display-list write can say otherwise — cover the grid.
    switch (uniAttn * 4 + uniDiff) {
    case 0 * 4 + 0:
        evalUniformLighting<0, 0>(chan, amb, mat, lights, uniMask, posView, nrmView, clr0, clr1, out0, out1);
        break;
    case 0 * 4 + 1:
        evalUniformLighting<0, 1>(chan, amb, mat, lights, uniMask, posView, nrmView, clr0, clr1, out0, out1);
        break;
    case 0 * 4 + 2:
        evalUniformLighting<0, 2>(chan, amb, mat, lights, uniMask, posView, nrmView, clr0, clr1, out0, out1);
        break;
    case 1 * 4 + 0:
        evalUniformLighting<1, 0>(chan, amb, mat, lights, uniMask, posView, nrmView, clr0, clr1, out0, out1);
        break;
    case 1 * 4 + 1:
        evalUniformLighting<1, 1>(chan, amb, mat, lights, uniMask, posView, nrmView, clr0, clr1, out0, out1);
        break;
    case 1 * 4 + 2:
        evalUniformLighting<1, 2>(chan, amb, mat, lights, uniMask, posView, nrmView, clr0, clr1, out0, out1);
        break;
    case 2 * 4 + 1:
        evalUniformLighting<2, 1>(chan, amb, mat, lights, uniMask, posView, nrmView, clr0, clr1, out0, out1);
        break;
    case 2 * 4 + 2:
        evalUniformLighting<2, 2>(chan, amb, mat, lights, uniMask, posView, nrmView, clr0, clr1, out0, out1);
        break;
    default:
        evalUniformLighting<2, 0>(chan, amb, mat, lights, uniMask, posView, nrmView, clr0, clr1, out0, out1);
        break;
    }
}

}  // namespace Platform::CompatGx
