#include "revolution/mtx.h"

extern f64 tan(f64);

void C_MTXPerspective(Mtx44 m, f32 fovY, f32 aspect, f32 n, f32 f) {
    f32 angle;
    f32 cot;
    f32 tmp;
    angle = fovY * 0.5f;
    angle = MTXDegToRad(angle);
    cot = tan(angle);
    cot = 1.0f / cot;

    m[0][0] = cot / aspect;
    m[0][1] = 0.0f;
    m[0][2] = 0.0f;
    m[0][3] = 0.0f;

    m[1][0] = 0.0f;
    m[1][1] = cot;
    m[1][2] = 0.0f;
    m[1][3] = 0.0f;

    m[2][0] = 0.0f;
    m[2][1] = 0.0f;

    tmp = 1.0f / (f - n);
    m[2][2] = -(n)*tmp;
    m[2][3] = -(f * n) * tmp;

    m[3][0] = 0.0f;
    m[3][1] = 0.0f;
    m[3][2] = -1.0f;
    m[3][3] = 0.0f;
}

void C_MTXOrtho(Mtx44 m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f) {
    f32 tmp;
    tmp = 1.0f / (r - l);
    m[0][0] = 2.0f * tmp;
    m[0][1] = 0.0f;
    m[0][2] = 0.0f;
    m[0][3] = -(r + l) * tmp;

    tmp = 1.0f / (t - b);
    m[1][0] = 0.0f;
    m[1][1] = 2.0f * tmp;
    m[1][2] = 0.0f;
    m[1][3] = -(t + b) * tmp;

    m[2][0] = 0.0f;
    m[2][1] = 0.0f;

    tmp = 1.0f / (f - n);
    m[2][2] = -(1.0f) * tmp;
    m[2][3] = -(f)*tmp;

    m[3][0] = 0.0f;
    m[3][1] = 0.0f;
    m[3][2] = 0.0f;
    m[3][3] = 1.0f;
}

// clang-format off
// PC_PORT PATCH (compat/patches): the original PSMTX44Identity is PPC asm
// inside a function with a register-asm block and PSMTX44Copy is a naked asm
// function (both Metrowerks-only). Host scalar equivalents:
//   - PSMTX44Identity: identity 4x4.
//   - PSMTX44Copy: memcpy 16 floats.
// (C_MTXOrtho/C_MTXPerspective above are untouched upstream code.)
//
// Also: the original asm is not guarded by __MWERKS__, so the vendored file
// cannot be compiled on a host toolchain without this patch.
// ============================================================================

void PSMTX44Identity(register Mtx44 m)
{
    f32 c1 = 1.0F;
    f32 c0 = 0.0F;
    // PC_PORT: scalar stores (the PPC asm wrote the same values).
    m[0][0] = c1; m[0][1] = c0; m[0][2] = c0; m[0][3] = c0;
    m[1][0] = c0; m[1][1] = c1; m[1][2] = c0; m[1][3] = c0;
    m[2][0] = c0; m[2][1] = c0; m[2][2] = c1; m[2][3] = c0;
    m[3][0] = c0; m[3][1] = c0; m[3][2] = c0; m[3][3] = c1;
}

void PSMTX44Copy(const register Mtx44 src, register Mtx44 dst)
{
    for (int i = 0; i < 16; ++i) {
        dst[0][i] = src[0][i];
    }
}

//clang-format on
