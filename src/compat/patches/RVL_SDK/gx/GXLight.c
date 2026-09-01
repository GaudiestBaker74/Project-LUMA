// =============================================================================
// PC_PORT patched copy of the vendored GXLight.c
// (third_party/petari/src/RVL_SDK/gx/GXLight.c).
//
// Only the pure light-object builders are kept (GXInitLight*): they write
// floats into the caller-owned GXLightObj struct (GXLightObjInt layout:
// reserved[3], Color, a[3], k[3], lpos[3], ldir[3]) and are endian-safe /
// host-compilable as-is.
//
// REMOVED vs the original (replaced by src/compat/gx/GXLight.cpp):
//   * ConvLightID2Num + WriteLightObjPS (PPC asm + __cntlzw) and
//     GXLoadLightObjImm — the compat layer reads the GXLightObj struct and
//     stores it in the channel-lighting mirror instead of writing XF regs.
//   * GXSetChanAmbColor / GXSetChanMatColor / GXSetNumChans / GXSetChanCtrl —
//     re-implemented as semantic mirrors in GXLight.cpp (no XF register
//     emulation needed; the mirror drives the CPU lighting evaluator).
// The only upstream difference is that removal; nothing else changed.
// =============================================================================

// C translation unit: <math.h>, not <cmath> (the vendored original uses
// <cmath> because the Wii build compiles it as C++).
#include <math.h>
#include <revolution/gx.h>

#define PI 3.14159265358979323846f
#define BIG_NUMBER 1.0E+18f

void GXInitLightAttn(GXLightObj* lt_obj, f32 a0, f32 a1, f32 a2, f32 k0, f32 k1, f32 k2) {
    GX_SETUP_LIGHT(obj, lt_obj)
    obj->a[0] = a0;
    obj->a[1] = a1;
    obj->a[2] = a2;
    obj->k[0] = k0;
    obj->k[1] = k1;
    obj->k[2] = k2;
}

void GXInitLightSpot(GXLightObj* lt_obj, f32 cutoff, GXSpotFn spot_func) {
    f32 r, d, cr, a0, a1, a2;
    GX_SETUP_LIGHT(obj, lt_obj)

    if (cutoff <= 0.0f || cutoff > 90.0f) {
        spot_func = GX_SP_OFF;
    }

    r = cutoff * PI / 180.0f;
    cr = cos(r);

    switch (spot_func) {
    case GX_SP_FLAT:
        a0 = -1000.0f * cr;
        a1 = 1000.0f;
        a2 = 0.0f;
        break;
    case GX_SP_COS:
        a1 = 1.0f / (1.0f - cr);
        a0 = -cr * a1;
        a2 = 0.0f;
        break;
    case GX_SP_COS2:
        a2 = 1.0f / (1.0f - cr);
        a0 = 0.0F;
        a1 = -cr * a2;
        break;
    case GX_SP_SHARP:
        d = 1.0f / ((1.0f - cr) * (1.0f - cr));
        a0 = cr * (cr - 2.0f) * d;
        a1 = 2.0f * d;
        a2 = -d;
        break;
    case GX_SP_RING1:
        d = 1.0f / ((1.0f - cr) * (1.0f - cr));
        a2 = -4.0f * d;
        a0 = a2 * cr;
        a1 = 4.0f * (1.0f + cr) * d;
        break;
    case GX_SP_RING2:
        d = 1.0f / ((1.0f - cr) * (1.0f - cr));
        a0 = 1.0f - 2.0f * cr * cr * d;
        a1 = 4.0f * cr * d;
        a2 = -2.0f * d;
        break;
    case GX_SP_OFF:
    default:
        a0 = 1.0f;
        a1 = 0.0f;
        a2 = 0.0f;
        break;
    }

    obj->a[0] = a0;
    obj->a[1] = a1;
    obj->a[2] = a2;
}

void GXInitLightDistAttn(GXLightObj* lt_obj, f32 ref_dist, f32 ref_br, GXDistAttnFn dist_func) {
    f32 k0, k1, k2;
    GX_SETUP_LIGHT(obj, lt_obj)

    if (ref_dist < 0.0f) {
        dist_func = GX_DA_OFF;
    }

    if (ref_br <= 0.0f || ref_br >= 1.0f) {
        dist_func = GX_DA_OFF;
    }

    switch (dist_func) {
    case GX_DA_GENTLE:
        k0 = 1.0f;
        k1 = (1.0f - ref_br) / (ref_br * ref_dist);
        k2 = 0.0f;
        break;
    case GX_DA_MEDIUM:
        k0 = 1.0f;
        k1 = 0.5f * (1.0f - ref_br) / (ref_br * ref_dist);
        k2 = 0.5f * (1.0f - ref_br) / (ref_br * ref_dist * ref_dist);
        break;
    case GX_DA_STEEP:
        k0 = 1.0f;
        k1 = 0.0f;
        k2 = (1.0f - ref_br) / (ref_br * ref_dist * ref_dist);
        break;
    case GX_DA_OFF:
    default:
        k0 = 1.0f;
        k1 = 0.0f;
        k2 = 0.0f;
        break;
    }

    obj->k[0] = k0;
    obj->k[1] = k1;
    obj->k[2] = k2;
}

void GXInitLightPos(GXLightObj* lt_obj, f32 x, f32 y, f32 z) {
    GX_SETUP_LIGHT(obj, lt_obj)
    obj->lpos[0] = x;
    obj->lpos[1] = y;
    obj->lpos[2] = z;
}

void GXInitLightDir(GXLightObj* lt_obj, f32 nx, f32 ny, f32 nz) {
    GX_SETUP_LIGHT(obj, lt_obj)
    obj->ldir[0] = -nx;
    obj->ldir[1] = -ny;
    obj->ldir[2] = -nz;
}

void GXInitSpecularDir(GXLightObj* lt_obj, f32 nx, f32 ny, f32 nz) {
    f32 mag;
    f32 vx, vy, vz;
    GX_SETUP_LIGHT(obj, lt_obj)
    vx = -nx;
    vy = -ny;
    vz = (-nz + 1.0f);
    mag = vx * vx + vy * vy + vz * vz;

    if (mag != 0.0f) {
        mag = 1.0f / (f32)sqrt(mag);
    }

    obj->ldir[0] = vx * mag;
    obj->ldir[1] = vy * mag;
    obj->ldir[2] = vz * mag;
    obj->lpos[0] = nx * -BIG_NUMBER;
    obj->lpos[1] = ny * -BIG_NUMBER;
    obj->lpos[2] = nz * -BIG_NUMBER;
}

void GXInitSpecularDirHA(GXLightObj* lt_obj, f32 nx, f32 ny, f32 nz, f32 hx, f32 hy, f32 hz) {
    GX_SETUP_LIGHT(obj, lt_obj)
    obj->ldir[0] = hx;
    obj->ldir[1] = hy;
    obj->ldir[2] = hz;
    obj->lpos[0] = nx * -BIG_NUMBER;
    obj->lpos[1] = ny * -BIG_NUMBER;
    obj->lpos[2] = nz * -BIG_NUMBER;
}

void GXInitLightColor(GXLightObj* lt_obj, GXColor color) {
    GX_SETUP_LIGHT(obj, lt_obj)
    obj->Color = *(u32*)(&color);
}
