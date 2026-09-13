// =============================================================================
// compat/j3d — BmdRenderer (see BmdRenderer.h).
// =============================================================================

#include "compat/j3d/BmdRenderer.h"

#include <revolution/gx.h>

#include <cmath>
#include <cstdlib>
#include <string>
#include <cstring>

#include "compat/gx/GXCompat.h"
#include "platform/Log/Log.h"

namespace compat::j3d {

namespace {

// J3DTexMtx::calcTexMtx's constant matrices (J3DTevs.cpp).
const Mtx kQMtx = {{0.5f, 0.0f, 0.5f, 0.0f}, {0.0f, -0.5f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f, 0.0f}};
const Mtx kQMtx2 = {{0.5f, 0.0f, 0.0f, 0.5f}, {0.0f, -0.5f, 0.0f, 0.5f}, {0.0f, 0.0f, 1.0f, 0.0f}};

GXColor toGxColor(const u8 c[4]) {
    GXColor out;
    out.r = c[0];
    out.g = c[1];
    out.b = c[2];
    out.a = c[3];
    return out;
}

bool isScaleOne(const f32 s[3]) {
    return s[0] == 1.0f && s[1] == 1.0f && s[2] == 1.0f;
}

} // namespace

BmdRenderer::BmdRenderer() {
    mtxIdentity(mBaseMtx);
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            mEffectMtx[r][c] = (r == c) ? 1.0f : 0.0f;
        }
    }
}

BmdRenderer::~BmdRenderer() = default;

bool BmdRenderer::init(std::vector<u8> bmdBytes, std::string* error) {
    mBytes = std::move(bmdBytes);
    mLoaded = mModel.load(mBytes.data(), mBytes.size(), error);
    if (!mLoaded) {
        return false;
    }
    mJointMtx.assign(mModel.joints.size(), MtxBox());
    mPose.resize(mModel.joints.size());
    for (size_t i = 0; i < mModel.joints.size(); ++i) {
        mPose[i] = mModel.joints[i].transform;
        mtxIdentity(mJointMtx[i].m);
    }
    mTexMtxOverride.assign(mModel.materials.size() * 8, TexMtxOverride());
    mTexObjs.assign(mModel.textures.size(), GXTexObj());
    mTlutObjs.assign(mModel.textures.size(), GXTlutObj());
    mTexObjInit.assign(mModel.textures.size(), false);
    PL_LOG_INFO("j3d", "bmd loaded: %zu joints, %zu shapes, %zu materials, %zu textures, %zu draw items",
                mModel.joints.size(), mModel.shapes.size(), mModel.materials.size(),
                mModel.textures.size(), mModel.drawItems.size());
    return true;
}

bool BmdRenderer::attachBtk(const std::vector<u8>& btkBytes, std::string* error) {
    if (!mLoaded) {
        if (error) {
            *error = "btk: model not loaded";
        }
        return false;
    }
    auto btk = std::make_unique<BtkAnim>();
    if (!btk->load(btkBytes.data(), btkBytes.size(), error)) {
        return false;
    }
    mBtkBindings.clear();
    for (auto& o : mTexMtxOverride) {
        o.active = false;
    }
    // J3DMaterialTable::entryTexMtxAnimator: bind by material NAME; the
    // texgen of the animated slot switches to TEXMTX(slot) and the material's
    // Maya flag/centre are taken from the BTK.
    for (size_t i = 0; i < btk->entries.size(); ++i) {
        const BtkEntry& e = btk->entries[i];
        const s32 mat = mModel.findMaterial(e.materialName.c_str());
        if (mat < 0 || e.texMtxSlot >= 8) {
            PL_LOG_WARN("j3d", "btk: entry '%s' slot %u has no matching material", e.materialName.c_str(),
                        e.texMtxSlot);
            continue;
        }
        BtkBinding b;
        b.material = mat;
        b.slot = e.texMtxSlot;
        b.entry = i;
        mBtkBindings.push_back(b);
        TexMtxOverride& o = mTexMtxOverride[static_cast<size_t>(mat) * 8 + e.texMtxSlot];
        o.active = true;
        o.maya = btk->maya;
        std::memcpy(o.center, e.center, sizeof(o.center));
        BmdMaterial& m = mModel.materials[mat];
        m.texCoord[e.texMtxSlot].mtx = static_cast<u8>(GX_TEXMTX0 + e.texMtxSlot * 3);
        if (!m.texMtx[e.texMtxSlot].valid) {
            m.texMtx[e.texMtxSlot].valid = true;  // J3D creates the tex mtx on demand
        }
    }
    mBtk = std::move(btk);
    mBtkCtrl.init(mBtk->loopMode, static_cast<f32>(mBtk->duration));
    PL_LOG_INFO("j3d", "btk attached: %zu entries (%zu bound), %u frames, loop %u", mBtk->entries.size(),
                mBtkBindings.size(), mBtk->duration, mBtk->loopMode);
    return true;
}

bool BmdRenderer::attachBck(const std::vector<u8>& bckBytes, std::string* error) {
    if (!mLoaded) {
        if (error) {
            *error = "bck: model not loaded";
        }
        return false;
    }
    auto bck = std::make_unique<BckAnim>();
    if (!bck->load(bckBytes.data(), bckBytes.size(), error)) {
        return false;
    }
    if (bck->joints.size() != mModel.joints.size()) {
        PL_LOG_WARN("j3d", "bck: %zu joint tracks for a %zu-joint model (extra tracks ignored)",
                    bck->joints.size(), mModel.joints.size());
    }
    mBck = std::move(bck);
    mBckCtrl.init(mBck->loopMode, static_cast<f32>(mBck->duration));
    PL_LOG_INFO("j3d", "bck attached: %zu joints, %u frames, loop %u", mBck->joints.size(), mBck->duration,
                mBck->loopMode);
    return true;
}

void BmdRenderer::update() {
    if (mBtk) {
        mBtkCtrl.update();
    }
    if (mBck) {
        mBckCtrl.update();
    }
}

void BmdRenderer::setBaseMtx(const Mtx m) {
    mtxCopy(m, mBaseMtx);
}

void BmdRenderer::setBaseScale(f32 x, f32 y, f32 z) {
    mBaseScale[0] = x;
    mBaseScale[1] = y;
    mBaseScale[2] = z;
}

void BmdRenderer::setEffectMtx(const Mtx44* m) {
    mHasEffectMtx = (m != nullptr);
    if (m) {
        std::memcpy(mEffectMtx, *m, sizeof(Mtx44));
    }
}

// --- joints ------------------------------------------------------------------

void BmdRenderer::calcJoints() {
    // Pose: JNT1 rest transform or the BCK frame.
    for (size_t i = 0; i < mModel.joints.size(); ++i) {
        if (mBck && i < mBck->joints.size()) {
            mBck->evaluate(i, mBckCtrl.frame, mPose[i]);
        } else {
            mPose[i] = mModel.joints[i].transform;
        }
    }
    // Root parent: base matrix scaled by the base scale (J3DMtxCalcJ3DSysInit
    // Basic/Maya: JMAMTXApplyScale(baseMtx, cur, baseScale)).
    Mtx rootParent;
    mtxApplyScale(mBaseMtx, rootParent, mBaseScale[0], mBaseScale[1], mBaseScale[2]);

    const u32 calcType = mModel.loadFlags & 0xF;  // 0 Basic, 1 SoftImage, 2 Maya
    std::vector<f32> accScale(mModel.joints.size() * 3, 1.0f);  // Basic: accumulated scale
    for (u16 idx : mModel.jointOrder) {
        if (idx >= mModel.joints.size()) {
            continue;
        }
        const BmdJoint& joint = mModel.joints[idx];
        const JointTransform& t = mPose[idx];
        Mtx local;
        getTranslateRotateMtx(t, local);
        const Mtx* parent = &rootParent;
        const s32 p = joint.parent;
        if (p >= 0 && static_cast<size_t>(p) < mJointMtx.size()) {
            parent = &mJointMtx[p].m;
        }
        if (calcType == 2) {
            // J3DMtxCalcCalcTransformMaya: local scale, optional scale
            // compensation against the parent's local scale.
            if (!isScaleOne(t.scale)) {
                mtxApplyScale(local, local, t.scale[0], t.scale[1], t.scale[2]);
            }
            if (joint.scaleCompensate == 1 && p >= 0 && static_cast<size_t>(p) < mPose.size()) {
                const f32* ps = mPose[p].scale;
                const f32 inv[3] = {ps[0] != 0.0f ? 1.0f / ps[0] : 1.0f, ps[1] != 0.0f ? 1.0f / ps[1] : 1.0f,
                                    ps[2] != 0.0f ? 1.0f / ps[2] : 1.0f};
                for (int r = 0; r < 3; ++r) {
                    for (int c = 0; c < 3; ++c) {
                        local[r][c] *= inv[r];
                    }
                }
            }
        } else {
            // J3DMtxCalcCalcTransformBasic: the scale accumulates down the
            // chain (mCurrentS) and is applied to the local matrix.
            f32 s[3] = {mBaseScale[0], mBaseScale[1], mBaseScale[2]};
            if (p >= 0) {
                s[0] = accScale[static_cast<size_t>(p) * 3 + 0];
                s[1] = accScale[static_cast<size_t>(p) * 3 + 1];
                s[2] = accScale[static_cast<size_t>(p) * 3 + 2];
            }
            s[0] *= t.scale[0];
            s[1] *= t.scale[1];
            s[2] *= t.scale[2];
            accScale[static_cast<size_t>(idx) * 3 + 0] = s[0];
            accScale[static_cast<size_t>(idx) * 3 + 1] = s[1];
            accScale[static_cast<size_t>(idx) * 3 + 2] = s[2];
            if (!isScaleOne(s)) {
                mtxApplyScale(local, local, t.scale[0], t.scale[1], t.scale[2]);
            }
        }
        mtxConcat(*parent, local, mJointMtx[idx].m);
    }
}

// --- texture matrices --------------------------------------------------------

void BmdRenderer::calcTexMtx(const BmdMaterial& mat, int slot, const Mtx modelMtx, const Mtx view,
                             Mtx out) const {
    const BmdTexMtx& tm = mat.texMtx[slot];
    const u32 mode = tm.info & 0x3F;
    bool maya = (tm.info >> 7) & 1;
    TexSrt srt = tm.srt;
    f32 center[3] = {tm.center[0], tm.center[1], tm.center[2]};
    const size_t matIdx = static_cast<size_t>(&mat - mModel.materials.data());
    if (matIdx < mModel.materials.size()) {
        const TexMtxOverride& o = mTexMtxOverride[matIdx * 8 + static_cast<size_t>(slot)];
        if (o.active) {
            srt = o.srt;
            maya = o.maya;
            std::memcpy(center, o.center, sizeof(center));
        }
    }

    // Input matrix (J3DTexGenBlockPatched::calc).
    Mtx input;
    switch (mode) {
    case 1:  // EnvmapBasic
    case 6:  // EnvmapOld
    case 7:  // Envmap
        mtxConcat(view, modelMtx, input);
        mtxZeroTranslation(input);
        break;
    case 2:  // ProjmapBasic
    case 8:  // Projmap
        mtxCopy(modelMtx, input);
        break;
    case 3:  // ViewProjmapBasic
    case 9:  // ViewProjmap
        mtxConcat(view, modelMtx, input);
        break;
    case 5:
    case 10: // EnvmapOldEffectMtx
    case 11: // EnvmapEffectMtx
        mtxCopy(modelMtx, input);
        mtxZeroTranslation(input);
        break;
    default:
        mtxIdentity(input);
        break;
    }
    // The runtime effect matrix (MR::initDLMakerProjmapEffectMtxSetter →
    // ProjmapEffectMtxSetter::updateMtxUseBaseMtx) does not REPLACE the
    // matrix baked into the BMD: it right-multiplies it by inverse(base) so
    // the projection keeps working in the model's own frame while the actor
    // (the sky dome) spins. Replacing it, as this port did, threw away the
    // sphere projection and flattened the sea into a wallpaper of the earth
    // map — see docs/title-widescreen.md.
    Mtx effectComposed;
    if (mHasEffectMtx) {
        mtxProjConcat(tm.effectMtx, mEffectMtx, effectComposed);
    } else {
        std::memcpy(effectComposed, tm.effectMtx, sizeof(Mtx));
    }
    const f32(*effect)[4] = effectComposed;
    // EXPERIMENT: a uniform texcoord scale for this material, applied to every
    // texgen (what the hardware's texcoord-scale register would do). The
    // reference frames sample the earth textures ONCE across the sea, while the
    // encoded SRT/texcoords produce tens of repeats, so the effective scale is
    // ~1/20 (LUMA_SKY_UV_SCALE tunes it while fitting).
    static const float kUvScale = [] {
        const char* e = std::getenv("LUMA_SKY_UV_SCALE");
        return (e != nullptr) ? static_cast<float>(std::atof(e)) : 1.0f;
    }();
    if (kUvScale != 1.0f && mat.name.find("Earth") != std::string::npos) {
        srt.scaleX *= kUvScale;
        srt.scaleY *= kUvScale;
    }

    // J3DTexMtx::calcTexMtx.
    Mtx srtMtx;
    Mtx tmp;
    switch (mode) {
    case 8:
    case 9:
    case 11:
        if (maya) {
            getTextureMtxMaya(srt, srtMtx);
        } else {
            getTextureMtx(srt, center, srtMtx);
        }
        mtxConcat(srtMtx, kQMtx, srtMtx);
        mtxProjConcat(srtMtx, effect, tmp);
        mtxConcat(tmp, input, out);
        break;
    case 7:
        if (maya) {
            getTextureMtxMaya(srt, srtMtx);
        } else {
            getTextureMtx(srt, center, srtMtx);
        }
        mtxConcat(srtMtx, kQMtx, srtMtx);
        mtxConcat(srtMtx, input, out);
        break;
    case 10:
        if (maya) {
            getTextureMtxMayaOld(srt, srtMtx);
        } else {
            getTextureMtxOld(srt, center, srtMtx);
        }
        mtxConcat(srtMtx, kQMtx2, srtMtx);
        mtxProjConcat(srtMtx, effect, tmp);
        mtxConcat(tmp, input, out);
        break;
    case 6:
        if (maya) {
            getTextureMtxMayaOld(srt, srtMtx);
        } else {
            getTextureMtxOld(srt, center, srtMtx);
        }
        mtxConcat(srtMtx, kQMtx2, srtMtx);
        mtxConcat(srtMtx, input, out);
        break;
    case 1:
        if (maya) {
            getTextureMtxMayaOld(srt, srtMtx);
        } else {
            getTextureMtxOld(srt, center, srtMtx);
        }
        mtxConcat(srtMtx, input, out);
        break;
    case 2:
    case 3:
    case 5:
        if (maya) {
            getTextureMtxMayaOld(srt, srtMtx);
        } else {
            getTextureMtxOld(srt, center, srtMtx);
        }
        mtxProjConcat(srtMtx, effect, tmp);
        mtxConcat(tmp, input, out);
        break;
    case 4:
        if (maya) {
            getTextureMtxMayaOld(srt, srtMtx);
        } else {
            getTextureMtxOld(srt, center, srtMtx);
        }
        mtxProjConcat(srtMtx, effect, out);
        break;
    default:
        if (maya) {
            getTextureMtxMayaOld(srt, srtMtx);
        } else {
            getTextureMtxOld(srt, center, srtMtx);
        }
        mtxCopy(srtMtx, out);
        break;
    }

    // M9.5.9 EXPERIMENT: the projmap traversal the console uses. Measured with
    // LUMA_GX_UV_LOG=1, the composed matrix sweeps ~10 Earth-map repeats per 45
    // degree sector while the reference shows the map about once, so the scale
    // belongs to the xy rows of the projection. Scaling the SRT (as the earlier
    // LUMA_SKY_UV_SCALE did) cannot work: the projective path takes the SRT
    // through kQMtx only, the traversal is set by the effect/projection matrix.
    static const float kProjScale = [] {
        const char* e = std::getenv("LUMA_SKY_PROJ_SCALE");
        return (e != nullptr) ? static_cast<float>(std::atof(e)) : 1.0f;
    }();
    if (kProjScale != 1.0f && (mode == 8 || mode == 9)) {
        for (int c = 0; c < 4; ++c) {
            out[0][c] *= kProjScale;
            out[1][c] *= kProjScale;
        }
    }
}

// --- textures ----------------------------------------------------------------

void BmdRenderer::bindTextures(const BmdMaterial& mat) {
    for (int i = 0; i < 8; ++i) {
        const s32 texNo = mat.texNo[i];
        if (texNo < 0 || static_cast<size_t>(texNo) >= mModel.textures.size()) {
            continue;
        }
        const BmdTexture& t = mModel.textures[texNo];
        if (!t.image) {
            continue;
        }
        GXTexObj& obj = mTexObjs[texNo];
        if (!mTexObjInit[texNo]) {
            // JUTTexture::init / J3DTexture: CI formats name a TLUT built from
            // the BTI palette; everything else is a plain object.
            const u8 fmt = t.header.format;
            const bool ci = (fmt == GX_TF_C4 || fmt == GX_TF_C8 || fmt == GX_TF_C14X2);
            if (ci && t.palette) {
                // PC_PORT: the compat TLUT registry is keyed by name; one name
                // per texture keeps palettes independent.
                const u32 tlutName = 0x4A3D0000u + static_cast<u32>(texNo);
                GXInitTlutObj(&mTlutObjs[texNo], const_cast<u8*>(t.palette),
                              static_cast<GXTlutFmt>(t.header.paletteFormat), t.header.paletteCount);
                GXLoadTlut(&mTlutObjs[texNo], tlutName);
                GXInitTexObjCI(&obj, const_cast<u8*>(t.image), t.header.width, t.header.height,
                               static_cast<GXCITexFmt>(fmt), static_cast<GXTexWrapMode>(t.header.wrapS),
                               static_cast<GXTexWrapMode>(t.header.wrapT), GX_FALSE, tlutName);
            } else {
                GXInitTexObj(&obj, const_cast<u8*>(t.image), t.header.width, t.header.height,
                             static_cast<GXTexFmt>(fmt), static_cast<GXTexWrapMode>(t.header.wrapS),
                             static_cast<GXTexWrapMode>(t.header.wrapT), t.mipmap ? GX_TRUE : GX_FALSE);
            }
            // M9.5.8: the image blob covers the mip chain (see BmdModel), so the
            // loader can upload the levels the LOD range samples.
            Platform::CompatGx::setTexObjImageBytes(&obj, t.imageBytes);
            GXInitTexObjLOD(&obj, static_cast<GXTexFilter>(t.minFilter), static_cast<GXTexFilter>(t.magFilter),
                            t.minLod / 8.0f, t.maxLod / 8.0f, t.lodBias / 100.0f,
                            t.biasClamp ? GX_TRUE : GX_FALSE, t.edgeLod ? GX_TRUE : GX_FALSE,
                            static_cast<GXAnisotropy>(t.maxAnisotropy));
            mTexObjInit[texNo] = true;
        }
        GXLoadTexObj(&obj, static_cast<GXTexMapID>(GX_TEXMAP0 + i));
    }
}

// --- materials ---------------------------------------------------------------

void BmdRenderer::applyMaterial(size_t matIndex, const Mtx modelMtx, const Mtx view) {
    const BmdMaterial& mat = mModel.materials[matIndex];

    // Colour block (J3DColorBlockLightOff/AmbientOn::load).
    GXSetNumChans(mat.chanNum);
    for (int i = 0; i < 2; ++i) {
        GXSetChanMatColor(static_cast<GXChannelID>(GX_COLOR0A0 + i), toGxColor(mat.matColor[i]));
        GXSetChanAmbColor(static_cast<GXChannelID>(GX_COLOR0A0 + i), toGxColor(mat.ambColor[i]));
    }
    // BMD channel order: COLOR0, ALPHA0, COLOR1, ALPHA1 → GX ids 0, 2, 1, 3.
    static const GXChannelID kChanIds[4] = {GX_COLOR0, GX_ALPHA0, GX_COLOR1, GX_ALPHA1};
    for (int i = 0; i < 4; ++i) {
        const BmdColorChan& ch = mat.chan[i];
        GXSetChanCtrl(kChanIds[i], ch.enable ? GX_TRUE : GX_FALSE, static_cast<GXColorSrc>(ch.ambSrc),
                      static_cast<GXColorSrc>(ch.matSrc), ch.lightMask, static_cast<GXDiffuseFn>(ch.diffuseFn),
                      static_cast<GXAttnFn>(ch.attnFn));
    }

    // Tex-gen block: matrices + generators (J3DTexGenBlockBasic::load).
    GXSetNumTexGens(mat.texGenNum);
    for (int i = 0; i < 8; ++i) {
        const BmdTexCoord& tc = mat.texCoord[i];
        if (mat.texMtx[i].valid && tc.mtx != GX_IDENTITY) {
            Mtx m;
            calcTexMtx(mat, i, modelMtx, view, m);
            GXLoadTexMtxImm(m, static_cast<u32>(GX_TEXMTX0 + i * 3),
                            mat.texMtx[i].projection == GX_MTX3x4 ? GX_MTX3x4 : GX_MTX2x4);
        }
        if (i < mat.texGenNum) {
            GXSetTexCoordGen2(static_cast<GXTexCoordID>(GX_TEXCOORD0 + i), static_cast<GXTexGenType>(tc.type),
                              static_cast<GXTexGenSrc>(tc.src), tc.mtx, GX_FALSE, GX_PTIDENTITY);
        }
    }

    // TEV block (J3DTevBlock*::load).
    bindTextures(mat);
    const u8 stages = mat.tevStageNum == 0 ? 1 : (mat.tevStageNum > 16 ? 16 : mat.tevStageNum);
    GXSetNumTevStages(stages);
    for (int i = 0; i < 3; ++i) {
        GXColorS10 c;
        c.r = mat.tevColor[i][0];
        c.g = mat.tevColor[i][1];
        c.b = mat.tevColor[i][2];
        c.a = mat.tevColor[i][3];
        GXSetTevColorS10(static_cast<GXTevRegID>(GX_TEVREG0 + i), c);
    }
    for (int i = 0; i < 4; ++i) {
        GXSetTevKColor(static_cast<GXTevKColorID>(GX_KCOLOR0 + i), toGxColor(mat.kColor[i]));
    }
    for (int i = 0; i < 4; ++i) {
        GXSetTevSwapModeTable(static_cast<GXTevSwapSel>(GX_TEV_SWAP0 + i),
                              static_cast<GXTevColorChan>(mat.swapTable[i][0]),
                              static_cast<GXTevColorChan>(mat.swapTable[i][1]),
                              static_cast<GXTevColorChan>(mat.swapTable[i][2]),
                              static_cast<GXTevColorChan>(mat.swapTable[i][3]));
    }
    for (int i = 0; i < stages; ++i) {
        const GXTevStageID st = static_cast<GXTevStageID>(GX_TEVSTAGE0 + i);
        const BmdTevOrder& o = mat.tevOrder[i];
        GXSetTevOrder(st, static_cast<GXTexCoordID>(o.texCoord), static_cast<GXTexMapID>(o.texMap),
                      static_cast<GXChannelID>(o.colorChan));
        const BmdTevStage& s = mat.tevStage[i];
        GXSetTevColorIn(st, static_cast<GXTevColorArg>(s.colorIn[0]), static_cast<GXTevColorArg>(s.colorIn[1]),
                        static_cast<GXTevColorArg>(s.colorIn[2]), static_cast<GXTevColorArg>(s.colorIn[3]));
        GXSetTevColorOp(st, static_cast<GXTevOp>(s.colorOp), static_cast<GXTevBias>(s.colorBias),
                        static_cast<GXTevScale>(s.colorScale), s.colorClamp ? GX_TRUE : GX_FALSE,
                        static_cast<GXTevRegID>(s.colorReg));
        GXSetTevAlphaIn(st, static_cast<GXTevAlphaArg>(s.alphaIn[0]), static_cast<GXTevAlphaArg>(s.alphaIn[1]),
                        static_cast<GXTevAlphaArg>(s.alphaIn[2]), static_cast<GXTevAlphaArg>(s.alphaIn[3]));
        GXSetTevAlphaOp(st, static_cast<GXTevOp>(s.alphaOp), static_cast<GXTevBias>(s.alphaBias),
                        static_cast<GXTevScale>(s.alphaScale), s.alphaClamp ? GX_TRUE : GX_FALSE,
                        static_cast<GXTevRegID>(s.alphaReg));
        GXSetTevKColorSel(st, static_cast<GXTevKColorSel>(mat.kColorSel[i]));
        GXSetTevKAlphaSel(st, static_cast<GXTevKAlphaSel>(mat.kAlphaSel[i]));
        GXSetTevSwapMode(st, static_cast<GXTevSwapSel>(mat.swapMode[i][0]),
                         static_cast<GXTevSwapSel>(mat.swapMode[i][1]));
        GXSetTevDirect(st);
    }
    GXSetNumIndStages(0);

    // Pixel-engine block (J3DPEBlock*::load).
    GXSetFog(static_cast<GXFogType>(mat.fog.type), mat.fog.startZ, mat.fog.endZ, mat.fog.nearZ, mat.fog.farZ,
             toGxColor(mat.fog.color));
    GXSetAlphaCompare(static_cast<GXCompare>(mat.alphaComp[0]), mat.alphaComp[1],
                      static_cast<GXAlphaOp>(mat.alphaComp[2]), static_cast<GXCompare>(mat.alphaComp[3]),
                      mat.alphaComp[4]);
    GXSetBlendMode(static_cast<GXBlendMode>(mat.blend[0]), static_cast<GXBlendFactor>(mat.blend[1]),
                   static_cast<GXBlendFactor>(mat.blend[2]), static_cast<GXLogicOp>(mat.blend[3]));
    GXSetZMode(mat.zMode[0] ? GX_TRUE : GX_FALSE, static_cast<GXCompare>(mat.zMode[1]),
               mat.zMode[2] ? GX_TRUE : GX_FALSE);
    GXSetZCompLoc(mat.zCompLoc ? GX_TRUE : GX_FALSE);
    GXSetDither(mat.dither ? GX_TRUE : GX_FALSE);
    // PC_PORT: GX front faces are clockwise in screen space; GXCompat.cpp
    // (cullModeFromGx) maps that onto the host renderer's CCW convention, so
    // the MAT3 cull mode is honoured exactly as J3DGDSetGenMode does on the
    // console. Callers that can afford it (TitleSky: opaque dome seen from
    // inside) may still override it with GX_CULL_NONE (see setCullModeOverride).
    const int cull = (mCullOverride >= 0 && mCullOverride <= GX_CULL_ALL) ? mCullOverride
                                                                          : static_cast<int>(mat.cullMode);
    GXSetCullMode(static_cast<GXCullMode>(cull));
}

// --- shapes ------------------------------------------------------------------

void BmdRenderer::drawShape(const BmdShape& shape, const Mtx view) {
    if (shape.hasMatrixIndexAttr || shape.mtxType == 3) {
        // PNMTXIDX per vertex (skinned/multi-matrix): the host DL interpreter
        // does not consume matrix-index attributes, so the FIFO would desync.
        if (!mWarnedSkinned) {
            PL_LOG_WARN("j3d", "bmd: skipping multi-matrix/skinned shape (mtxType %u) — not supported on the host",
                        shape.mtxType);
            mWarnedSkinned = true;
        }
        return;
    }
    // VCD/VAT from the SHP1 declaration + VTX1 formats (J3DShape::makeVcdVatCmd).
    GXClearVtxDesc();
    for (const BmdVtxDecl& d : shape.decls) {
        const BmdVertexArray* arr = mModel.array(d.attr);
        if (!arr) {
            continue;
        }
        GXSetVtxDesc(static_cast<GXAttr>(d.attr), static_cast<GXAttrType>(d.type));
        GXSetVtxAttrFmt(GX_VTXFMT0, static_cast<GXAttr>(d.attr), static_cast<GXCompCnt>(arr->compCnt),
                        static_cast<GXCompType>(arr->compType), arr->frac);
        GXSetArray(static_cast<GXAttr>(d.attr), arr->data.data(), arr->stride);
    }
    for (const BmdMtxGroup& g : shape.groups) {
        // Draw matrix: the first table entry (single-matrix shapes have one).
        Mtx world;
        mtxIdentity(world);
        if (!g.mtxTable.empty() && g.mtxTable[0] != 0xFFFF && g.mtxTable[0] < mModel.drawMatrices.size()) {
            const BmdDrawMatrix& dm = mModel.drawMatrices[g.mtxTable[0]];
            if (!dm.weighted && dm.index < mJointMtx.size()) {
                mtxCopy(mJointMtx[dm.index].m, world);
            }
        }
        Mtx pos;
        mtxConcat(view, world, pos);
        Mtx nrm;
        mtxNormalMtx(pos, nrm);
        GXLoadPosMtxImm(pos, GX_PNMTX0);
        GXLoadNrmMtxImm(nrm, GX_PNMTX0);
        GXSetCurrentMtx(GX_PNMTX0);
        if (g.dl && g.dlSize) {
            GXCallDisplayList(g.dl, g.dlSize);
        }
    }
    ++mLastDrawnShapes;
}

void BmdRenderer::draw(const Mtx view) {
    if (!mLoaded) {
        return;
    }
    mLastDrawnShapes = 0;
    calcJoints();

    // BTK: evaluate the bound entries for this frame.
    if (mBtk) {
        for (const BtkBinding& b : mBtkBindings) {
            TexMtxOverride& o = mTexMtxOverride[static_cast<size_t>(b.material) * 8 + b.slot];
            mBtk->evaluate(b.entry, mBtkCtrl.frame, o.srt);
        }
    }

    if (mModel.drawItems.empty()) {
        if (!mWarnedNoShapes) {
            PL_LOG_WARN("j3d", "bmd: hierarchy has no shapes to draw");
            mWarnedNoShapes = true;
        }
        return;
    }
    // Two passes like J3DDrawBuffer OPA/XLU.
    for (int pass = 0; pass < 2; ++pass) {
        for (const BmdDrawItem& item : mModel.drawItems) {
            if (item.material >= mModel.materials.size() || item.shape >= mModel.shapes.size()) {
                continue;
            }
            const BmdMaterial& mat = mModel.materials[item.material];
            // TEMP DEBUG (M9.5.4 diagnostics): isolate draw items from the
            // environment, e.g. LUMA_SKY_ONLY_MAT=Earth to see just the sea.
            {
                static const char* only = std::getenv("LUMA_SKY_ONLY_MAT");
                if (only != nullptr && mat.name.find(only) == std::string::npos) {
                    continue;
                }
                static const char* hide = std::getenv("LUMA_SKY_HIDE_MAT");
                if (hide != nullptr && mat.name.find(hide) != std::string::npos) {
                    continue;
                }
            }
            const bool xlu = (mat.mode == 4);
            if ((pass == 0) == xlu) {
                continue;
            }
            const Mtx* modelMtx = (static_cast<size_t>(mat.joint) < mJointMtx.size())
                                      ? &mJointMtx[mat.joint].m
                                      : &mJointMtx[item.joint < mJointMtx.size() ? item.joint : 0].m;
            applyMaterial(item.material, *modelMtx, view);
            drawShape(mModel.shapes[item.shape], view);
        }
    }
}

} // namespace compat::j3d
