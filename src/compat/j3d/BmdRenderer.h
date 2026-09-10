#pragma once
// =============================================================================
// compat/j3d — host renderer for BmdModel through the compat GX API
// (M9.5.4 v8).
//
// Replays what the J3D runtime does per frame for a non-skinned model
// (J3DModel::calc → calcMaterial → J3DDrawBuffer draw):
//   * joint matrices: JNT1 transform (or the BCK pose) composed
//     parent-before-child under the caller's base matrix + base scale
//     (J3DMtxCalcCalcTransformBasic/Maya; scale compensation honoured),
//   * per material: colour channels (GXSetChanCtrl/MatColor/AmbColor), tex
//     gens + texture matrices (J3DTexGenBlockPatched::calc + J3DTexMtx::
//     calcTexMtx, incl. the effect-matrix projection modes), textures
//     (GXInitTexObj/LOD/CI + GXLoadTexObj on TEX1 entries), the TEV stages,
//     konst/registers, swap tables, alpha compare, blend, Z mode, cull, fog,
//   * per shape: GXSetVtxDesc/GXSetVtxAttrFmt from the SHP1 declaration,
//     GXSetArray on the VTX1 arrays, GXLoadPosMtxImm/NrmMtxImm(view * joint),
//     then GXCallDisplayList on every packet display list.
// Opaque materials draw first, translucent (mode 4) ones after, in hierarchy
// order — the two J3DDrawBuffer passes of MR::DrawBufferType_Sky.
//
// Not covered: envelope skinning (EVP1), multi-matrix shapes (PNMTXIDX in the
// packet), indirect stages, billboards keep their joint matrix. Shapes that
// need these are skipped with a one-time warning.
// =============================================================================

#include <revolution/types.h>
#include <revolution/mtx.h>
#include <revolution/gx/GXStruct.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "compat/j3d/BckAnim.h"
#include "compat/j3d/BmdModel.h"
#include "compat/j3d/BtkAnim.h"
#include "compat/j3d/J3DMathCompat.h"

namespace compat::j3d {

class BmdRenderer {
public:
    BmdRenderer();
    ~BmdRenderer();

    /// Takes ownership of the model bytes (the parser keeps pointers into
    /// them). Returns false when parsing fails.
    bool init(std::vector<u8> bmdBytes, std::string* error = nullptr);
    /// Attaches a BTK (texture SRT animation); the bytes are consumed.
    bool attachBtk(const std::vector<u8>& btkBytes, std::string* error = nullptr);
    /// Attaches a BCK (joint animation); the bytes are consumed.
    bool attachBck(const std::vector<u8>& bckBytes, std::string* error = nullptr);

    /// Advances the attached animations by one frame (60 Hz game frame).
    void update();

    /// Base transform (LiveActor base matrix) and scale (mScale), applied
    /// under every joint like J3DModel::setBaseTRMtx/setBaseScale.
    void setBaseMtx(const Mtx m);
    void setBaseScale(f32 x, f32 y, f32 z);
    /// Effect matrix override for the Projmap/EffectMtx texgen modes. Passing
    /// nullptr restores the per-material MAT3 effect matrices.
    /// ProjmapEffectMtxSetter::updateMtxUseBaseMtx sets inverse(baseMtx).
    void setEffectMtx(const Mtx44* m);
    /// PC_PORT: overrides the MAT3 cull mode of every material (GX_CULL_NONE
    /// = draw both faces); `-1` restores MAT3. The GX front-face convention
    /// (clockwise) is mapped by the compat GX layer (cullModeFromGx, pinned
    /// by gx_cull_front_face_is_clockwise), so materials normally need no
    /// help; TitleSky still forces both faces as a belt-and-braces measure —
    /// for an opaque dome seen from inside the result is identical.
    void setCullModeOverride(int gxCullMode) { mCullOverride = gxCullMode; }
    int cullModeOverride() const { return mCullOverride; }

    /// Recomputes joint matrices, texture matrices and draws the model. `view`
    /// is the camera matrix (world → view, GX convention); the projection must
    /// already be set with GXSetProjection by the caller.
    void draw(const Mtx view);

    const BmdModel& model() const { return mModel; }
    bool loaded() const { return mLoaded; }
    /// Number of shapes drawn by the last draw() (diagnostics/tests).
    u32 lastDrawnShapes() const { return mLastDrawnShapes; }
    /// Current BTK/BCK frame (diagnostics/tests).
    f32 btkFrame() const { return mBtkCtrl.frame; }
    f32 bckFrame() const { return mBckCtrl.frame; }
    /// Evaluated joint → world matrix (after draw()).
    const Mtx* jointMtx(size_t i) const {
        return i < mJointMtx.size() ? &mJointMtx[i].m : nullptr;
    }

private:
    struct MtxBox {
        Mtx m;
    };
    struct BtkBinding {
        s32 material = -1;
        u8 slot = 0;
        size_t entry = 0;
    };

    void calcJoints();
    void calcTexMtx(const BmdMaterial& mat, int slot, const Mtx modelMtx, const Mtx view,
                    Mtx out) const;
    void applyMaterial(size_t matIndex, const Mtx modelMtx, const Mtx view);
    void drawShape(const BmdShape& shape, const Mtx view);
    void bindTextures(const BmdMaterial& mat);

    std::vector<u8> mBytes;
    BmdModel mModel;
    bool mLoaded = false;

    Mtx mBaseMtx;
    f32 mBaseScale[3] = {1.0f, 1.0f, 1.0f};
    bool mHasEffectMtx = false;
    Mtx44 mEffectMtx;

    std::vector<MtxBox> mJointMtx;         // joint → world
    std::vector<JointTransform> mPose;     // current per-joint transform

    std::unique_ptr<BtkAnim> mBtk;
    FrameCtrl mBtkCtrl;
    std::vector<BtkBinding> mBtkBindings;
    // Per material/slot: animated SRT + centre + Maya flag while a BTK drives it.
    struct TexMtxOverride {
        bool active = false;
        TexSrt srt;
        f32 center[3] = {0.5f, 0.5f, 0.5f};
        bool maya = false;
    };
    std::vector<TexMtxOverride> mTexMtxOverride;   // materials * 8

    std::unique_ptr<BckAnim> mBck;
    FrameCtrl mBckCtrl;

    std::vector<GXTexObj> mTexObjs;         // one per TEX1 entry (stable addresses)
    std::vector<GXTlutObj> mTlutObjs;
    std::vector<bool> mTexObjInit;
    u32 mLastDrawnShapes = 0;
    int mCullOverride = -1;                 // GXCullMode or -1 (use MAT3)
    bool mWarnedSkinned = false;
    bool mWarnedNoShapes = false;
};

} // namespace compat::j3d
