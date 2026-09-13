// =============================================================================
// compat/j3d — TitleSky (see TitleSky.h).
// =============================================================================

#include "compat/j3d/TitleSky.h"

#include <revolution/gx.h>

#include <JSystem/JKernel/JKRArchive.hpp>
#include <JSystem/JKernel/JKRMemArchive.hpp>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "Game/Util/FileUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "compat/game/UiAnchoring.h"
#include "compat/j3d/BmdRenderer.h"
#include "platform/Log/Log.h"

namespace compat::j3d {

namespace {

constexpr f32 kPi = 3.14159265358979323846f;
constexpr f32 kAngleIncY = 0.001f;   // FileSelectSky.cpp cAngleIncY
constexpr f32 kCycleX = 3000.0f;     // FileSelectSky.cpp cCycleX
constexpr f32 kScale = 0.8f;

// EXPERIMENT (M9.5.5): the scene's world scale. The authored tex-matrix data
// carries its projector constant (MAT3: effect row 2 = (0, -1, 0, 7000)), so
// the title's scene really lives in units of a few thousand — the dome's raw
// vertex data is +-785133 and the model matrix shrinks it into that range.
// Scaling the model matrix AND the camera by the same factor leaves the screen
// composition untouched (view/projection ratio unchanged) while scaling the
// texgen input (modelMtx x POS) — which is exactly the quantity the sea's
// texture coordinates come from. LUMA_SKY_SCENE_SCALE tunes it while fitting.
static f32 sceneScale() {
    static const f32 s = [] {
        const char* e = std::getenv("LUMA_SKY_SCENE_SCALE");
        return (e != nullptr) ? static_cast<f32>(std::atof(e)) : 1.0f;
    }();
    return s;
}
// FileSelectCameraController::exeTitle: cFarTarget/cFarPoint + 15000 on Y.
constexpr f32 kCamPos[3] = {0.0f, 15000.0f, 15000.0f};
constexpr f32 kCamTarget[3] = {0.0f, 15800.0f, 0.0f};
constexpr f32 kCamUp[3] = {0.0f, 1.0f, 0.0f};
constexpr f32 kTitleFovy = 60.0f;

// Field of view: CameraContext::makePerspective(fovy, aspect, 100, 800000) with
// the title camera's fovy (FileSelectCameraController::cTitleFovy). It is a
// VERTICAL field of view, and the aspect ratio comes from the framebuffer
// (CameraContext::getAspect()), so the console's 16:9 mode simply shows more
// sky to the sides than its 4:3 mode at the same vertical framing. The host
// follows the same rule with the host framebuffer (the window): 16:9 reproduces
// the reference capture exactly, and wider windows reveal more sky instead of
// magnifying the dome — no FOV "lock" is needed (or wanted).
// CameraContext defaults.
constexpr f32 kNearZ = 100.0f;
constexpr f32 kFarZ = 800000.0f;

bool endsWith(const char* s, const char* suffix) {
    const size_t n = std::strlen(s);
    const size_t m = std::strlen(suffix);
    if (m > n) {
        return false;
    }
    for (size_t i = 0; i < m; ++i) {
        char a = s[n - m + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<char>(a - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

std::string baseName(const char* name) {
    std::string s(name);
    const size_t slash = s.find_last_of('/');
    if (slash != std::string::npos) {
        s = s.substr(slash + 1);
    }
    const size_t dot = s.find_last_of('.');
    if (dot != std::string::npos) {
        s = s.substr(0, dot);
    }
    return s;
}

struct ArcFile {
    std::string name;
    std::vector<u8> bytes;
};

/// Copies every file of the archive matching one of the extensions.
void collectFiles(JKRArchive* arc, const char* const* exts, std::vector<ArcFile>& out) {
    for (u32 i = 0;; ++i) {
        JKRArchive::SDirEntry dir;
        if (!arc->getDirEntry(&dir, i)) {
            break;
        }
        if (((dir.mFileFlag >> JKRArchive::FILE_FLAG_FOLDER_SHIFT) & 1) != 0 || dir.mName == nullptr) {
            continue;
        }
        bool match = false;
        for (const char* const* e = exts; *e; ++e) {
            if (endsWith(dir.mName, *e)) {
                match = true;
                break;
            }
        }
        if (!match) {
            continue;
        }
        void* res = arc->getIdxResource(i);
        if (!res) {
            continue;
        }
        const s32 size = arc->getResSize(res);
        if (size <= 0) {
            continue;
        }
        ArcFile f;
        f.name = dir.mName;
        f.bytes.assign(static_cast<const u8*>(res), static_cast<const u8*>(res) + size);
        out.push_back(std::move(f));
    }
}

} // namespace

TitleSky::TitleSky() {
    mtxIdentity(mBaseMtx);
}

TitleSky::~TitleSky() = default;

f32 TitleSky::calcAngleX(u32 step) {
    // exeWait: step = nerveStep * PI / cCycleX; angleX = (1 - JMACosShort(step * 8)) * 1.5 * PI / 4.
    // JMACosShort takes s16 angle units: the float `step * 8` is converted to
    // s16 by the implicit conversion (truncation), so the effective argument
    // is a tiny integer count of 1/65536-turn units — the tilt is essentially
    // static (~0) on the console too. Reproduce that exactly.
    f32 s = static_cast<f32>(step) * kPi / kCycleX;
    if (s < 0.0f) {
        s = -s;
    }
    const s16 units = static_cast<s16>(static_cast<s32>(s * 8.0f));
    const f32 c = cosShort(units);
    return (1.0f - c) * 3.0f / 2.0f * kPi / 4.0f;
}

void TitleSky::calcBaseMtx(f32 angleX, f32 angleY, Mtx out) {
    // rotateY.makeRotate((0,1,0), angleY); rotateX.makeRotate((1,0,0), angleX);
    // base = rotateY * rotateX; base = inverse(base).
    Mtx rotY, rotX, m;
    mtxRotAxisRad(rotY, 0.0f, 1.0f, 0.0f, angleY);
    mtxRotAxisRad(rotX, 1.0f, 0.0f, 0.0f, angleX);
    mtxConcat(rotY, rotX, m);
    mtxInverse(m, out);
}

bool TitleSky::init(const char* archivePath) {
    mLoaded = false;
    mRenderer.reset();
    JKRMemArchive* arc = MR::mountArchive(archivePath, nullptr);
    if (arc == nullptr) {
        PL_LOG_WARN("j3d", "TitleSky: cannot mount '%s' — keeping the host backdrop", archivePath);
        return false;
    }
    static const char* const kModelExts[] = {".bdl", ".bmd", nullptr};
    static const char* const kBtkExts[] = {".btk", nullptr};
    static const char* const kBckExts[] = {".bck", nullptr};
    std::vector<ArcFile> models, btks, bcks;
    collectFiles(arc, kModelExts, models);
    collectFiles(arc, kBtkExts, btks);
    collectFiles(arc, kBckExts, bcks);
    if (models.empty()) {
        PL_LOG_WARN("j3d", "TitleSky: '%s' holds no bmd/bdl", archivePath);
        return false;
    }
    // MR::initModelManagerWithAnm("CometNearOrbitSky"): the model named after
    // the actor, else the first one.
    size_t pick = 0;
    for (size_t i = 0; i < models.size(); ++i) {
        if (baseName(models[i].name.c_str()) == "CometNearOrbitSky") {
            pick = i;
            break;
        }
    }
    mModelName = baseName(models[pick].name.c_str());
    auto renderer = std::make_unique<BmdRenderer>();
    std::string error;
    if (!renderer->init(std::move(models[pick].bytes), &error)) {
        PL_LOG_WARN("j3d", "TitleSky: '%s' failed to parse: %s", models[pick].name.c_str(), error.c_str());
        return false;
    }
    // MR::startBtk / startBck(this, "CometNearOrbitSky").
    const auto attachNamed = [&](std::vector<ArcFile>& files, bool btk) {
        if (files.empty()) {
            return;
        }
        size_t idx = 0;
        for (size_t i = 0; i < files.size(); ++i) {
            if (baseName(files[i].name.c_str()) == "CometNearOrbitSky") {
                idx = i;
                break;
            }
        }
        std::string err;
        const bool ok = btk ? renderer->attachBtk(files[idx].bytes, &err) : renderer->attachBck(files[idx].bytes, &err);
        if (!ok) {
            PL_LOG_WARN("j3d", "TitleSky: '%s' not attached: %s", files[idx].name.c_str(), err.c_str());
        }
    };
    attachNamed(btks, true);
    attachNamed(bcks, false);

    renderer->setBaseScale(kScale * sceneScale(), kScale * sceneScale(), kScale * sceneScale());
    // PC_PORT: the dome is opaque and viewed from the inside, so drawing both
    // faces is visually identical to the console's GX_CULL_BACK (verified with
    // the synthetic dome both ways) — kept as a belt-and-braces measure for
    // the first run with the real asset. Pass -1 to honour MAT3 instead.
    renderer->setCullModeOverride(GX_CULL_NONE);
    // Diagnostics: GALAXY_SKY_CULL=mat3 honours the material cull mode (used to
    // validate the GX front-face mapping against a model wound like Nintendo's).
    if (const char* env = std::getenv("GALAXY_SKY_CULL")) {
        if (std::strcmp(env, "mat3") == 0) {
            renderer->setCullModeOverride(-1);
            PL_LOG_INFO("j3d", "TitleSky: GALAXY_SKY_CULL=mat3 — honouring the MAT3 cull mode");
        }
    }
    mRenderer = std::move(renderer);
    mStep = 0;
    mAngleX = 0.0f;
    mAngleY = 0.0f;
    calcBaseMtx(mAngleX, mAngleY, mBaseMtx);
    mLoaded = true;
    PL_LOG_INFO("j3d", "TitleSky: '%s' ready (%s, %zu btk, %zu bck)", archivePath, mModelName.c_str(),
                btks.size(), bcks.size());

    // PC_PORT diagnostic: what the dome is actually made of. The composition
    // (where the sea band, the horizon and the sun sit in the frame) is decided
    // by this geometry plus the camera, so the report logs the model's extent
    // and the material contract of every draw item:
    //   * bbox: the model's bounds around the origin the camera looks at (the
    //     actor's base matrix is rotation-only),
    //   * mtxType: 0 single / 1-2 billboard / 3 multi-matrix (3 is not drawn),
    //   * texGens + slot 0 (type/src/matrix) and the tex matrix mode
    //     (info & 0x3F: 8/9 projmap, 10/11 envmap+effect mtx -> the projector
    //     matrix TitleSky supplies) with its projection (0 = 3x4, 1 = 2x4),
    //   * cull / blend / z, so a wrong pass or cull mode is visible at a glance.
    {
        const BmdModel& mdl = mRenderer->model();
        PL_LOG_INFO("j3d",
                    "TitleSky model: %u verts, %zu joints, %zu shapes, %zu materials, %zu textures, "
                    "%zu draw items",
                    static_cast<unsigned>(mdl.vertexCount), mdl.joints.size(), mdl.shapes.size(),
                    mdl.materials.size(), mdl.textures.size(), mdl.drawItems.size());
        for (size_t di = 0; di < mdl.drawItems.size() && di < 24; ++di) {
            const BmdDrawItem& it = mdl.drawItems[di];
            if (it.shape >= mdl.shapes.size() || it.material >= mdl.materials.size()) {
                continue;
            }
            const BmdShape& sh = mdl.shapes[it.shape];
            const BmdMaterial& mt = mdl.materials[it.material];
            PL_LOG_INFO("j3d",
                        "  draw %zu: material '%s' shape %u joint %u mtxType=%u groups=%zu "
                        "bbox=(%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f) texGens=%u tc0=(type=%u src=%u mtx=%u) "
                        "tm0=(valid=%d mode=%u proj=%u) tex0=%d cull=%u mode=%u blend=(%u,%u,%u) "
                        "z=(%u,%u,%u)",
                        di, mt.name.c_str(), static_cast<unsigned>(it.shape),
                        static_cast<unsigned>(it.joint), static_cast<unsigned>(sh.mtxType),
                        sh.groups.size(), sh.bboxMin[0], sh.bboxMin[1], sh.bboxMin[2], sh.bboxMax[0],
                        sh.bboxMax[1], sh.bboxMax[2], static_cast<unsigned>(mt.texGenNum),
                        static_cast<unsigned>(mt.texCoord[0].type),
                        static_cast<unsigned>(mt.texCoord[0].src),
                        static_cast<unsigned>(mt.texCoord[0].mtx), static_cast<int>(mt.texMtx[0].valid),
                        static_cast<unsigned>(mt.texMtx[0].info & 0x3F),
                        static_cast<unsigned>(mt.texMtx[0].projection), static_cast<int>(mt.texNo[0]),
                        static_cast<unsigned>(mt.cullMode), static_cast<unsigned>(mt.mode),
                        static_cast<unsigned>(mt.blend[0]), static_cast<unsigned>(mt.blend[1]),
                        static_cast<unsigned>(mt.blend[2]), static_cast<unsigned>(mt.zMode[0]),
                        static_cast<unsigned>(mt.zMode[1]), static_cast<unsigned>(mt.zMode[2]));
        }
        for (size_t ti = 0; ti < mdl.textures.size() && ti < 8; ++ti) {
            const BmdTexture& tx = mdl.textures[ti];
            PL_LOG_INFO("j3d", "  texture %zu '%s': %ux%u fmt=0x%x wrap=(%u,%u) mip=%u", ti,
                        tx.name.c_str(), static_cast<unsigned>(tx.header.width),
                        static_cast<unsigned>(tx.header.height),
                        static_cast<unsigned>(tx.header.format),
                        static_cast<unsigned>(tx.header.wrapS),
                        static_cast<unsigned>(tx.header.wrapT), static_cast<unsigned>(tx.mipmap));
        }
    }
    return true;
}

void TitleSky::update() {
    if (!mLoaded || !mRenderer) {
        return;
    }
    // FileSelectSky::exeWait (the nerve step counts frames since appear): the
    // angles are advanced FIRST and the base matrix is built from the new ones —
    // the console applies the rotation of the frame it is computing, not the
    // previous one.
    mAngleX = calcAngleX(mStep);
    mAngleY += kAngleIncY;
    if (mAngleY > 2.0f * kPi) {
        mAngleY -= 2.0f * kPi;
    }
    ++mStep;
    // DIAGNOSTIC (LUMA_SKY_ANGLE_X_DEG / LUMA_SKY_ANGLE_Y_DEG): freeze the bob at
    // a phase so a capture can be compared with a still whose phase is known.
    {
        static const char* ex = std::getenv("LUMA_SKY_ANGLE_X_DEG");
        static const char* ey = std::getenv("LUMA_SKY_ANGLE_Y_DEG");
        if (ex != nullptr) {
            mAngleX = static_cast<f32>(std::atof(ex)) * (kPi / 180.0f);
        }
        if (ey != nullptr) {
            mAngleY = static_cast<f32>(std::atof(ey)) * (kPi / 180.0f);
        }
    }
    calcBaseMtx(mAngleX, mAngleY, mBaseMtx);
    // LiveActor::calcAnim: advance the model's animations, then
    // ProjmapEffectMtxSetter::updateMtxUseBaseMtx.
    mRenderer->update();
}

void TitleSky::draw() {
    if (!mLoaded || !mRenderer) {
        return;
    }
    // Camera: the whole look-at of FileSelectCameraController::exeTitle. The
    // view translation must stay: FileSelectSky::calcAndSetBaseMtx sets a
    // ROTATION-ONLY base matrix (exeWait inverts rotY*rotX, whose translation is
    // zero), so the dome sits at the world origin and it is the camera's
    // position — (0, 15000, 15000) looking at (0, 15800, 0) — that places the
    // limb, the sea band and the sun in the frame. An earlier PC_PORT shortcut
    // zeroed this translation ("sky actors follow the camera"), which put the
    // camera at the dome's centre and changed the whole composition.
    // The title camera's pitch. The console still places the WHOLE composition
    // ~55 px lower at 720p than the plain look-at of FileSelectCameraController's
    // title nerve gives: the teal band's first row, the horizon glow on the left
    // and the glare object on the right are all offset by the same amount, i.e.
    // the console's camera looks ~5 deg further up. (The dome's own angleX is ~0
    // there too: JMACosShort(step * 8) truncates to a tiny integer, so the bob
    // is vertical, not a tilt.) Measured at 1280x720 with this pitch: the teal
    // band starts at row 438 (console 438) and the two glare objects land at
    // (1086,475)/(153,481) (console (1081,477)/(165,478)).
    // LUMA_SKY_CAM_PITCH_DEG overrides it, which is how the value was fitted.
    static const f32 camPitchDeg = [] {
        const char* e = std::getenv("LUMA_SKY_CAM_PITCH_DEG");
        return (e != nullptr) ? static_cast<f32>(std::atof(e)) : 5.1f;
    }();
    Mtx view;
    const f32 ss = sceneScale();
    const f32 camPos[3] = {kCamPos[0] * ss, kCamPos[1] * ss, kCamPos[2] * ss};
    const f32 camTarget[3] = {kCamTarget[0] * ss, kCamTarget[1] * ss, kCamTarget[2] * ss};
    mtxLookAt(view, camPos, kCamUp, camTarget);
    if (camPitchDeg != 0.0f) {
        Mtx rot, pitched;
        mtxRotAxisRad(rot, 1.0f, 0.0f, 0.0f, -camPitchDeg * (kPi / 180.0f));
        mtxConcat(rot, view, pitched);
        mtxCopy(pitched, view);
    }

    // Projection: vertical fovy 60 (the title camera's) with the host
    // framebuffer's aspect ratio — see the note above the constants.
    f32 fbWidth = 0.0f;
    f32 fbHeight = 0.0f;
    compat::ui::framebufferSize(&fbWidth, &fbHeight);
    const f32 aspect = (fbHeight > 0.0f) ? fbWidth / fbHeight : (16.0f / 9.0f);

    static const f32 fovy = [] {
        const char* e = std::getenv("LUMA_SKY_FOVY");
        return (e != nullptr) ? static_cast<f32>(std::atof(e)) : kTitleFovy;
    }();
    Mtx44 proj;
    C_MTXPerspective(proj, fovy, aspect, kNearZ, kFarZ);
    GXSetProjection(proj, GX_PERSPECTIVE);

    // PC_PORT diagnostic (draw #1 and every 600 draws): the framing the dome was
    // drawn with, so a capture can be checked against the console still. It also
    // swings about X with a 750-step period (FileSelectSky::exeWait), so the step
    // and the angles belong with the picture. (The viewport rect the pass ran
    // with is logged by GXSetProjection when the projection is set.)
    static u32 sDrawCount = 0;
    ++sDrawCount;
    if (sDrawCount == 1 || sDrawCount % 600 == 0) {
        PL_LOG_INFO("j3d",
                    "TitleSky draw #%u: fb=%.1fx%.1f fovy=%.1f aspect=%.4f angleX=%.4f "
                    "angleY=%.4f step=%u",
                    static_cast<unsigned>(sDrawCount), fbWidth, fbHeight, kTitleFovy, aspect,
                    static_cast<double>(mAngleX), static_cast<double>(mAngleY),
                    static_cast<unsigned>(mStep));
    }

    // Base matrix + effect matrix. ProjmapEffectMtxSetter::updateMtxUseBaseMtx
    // feeds inverse(base) to the projmap materials; BmdRenderer composes it
    // with the matrix baked into the BMD instead of replacing it.
    mRenderer->setBaseMtx(mBaseMtx);
    Mtx inv;
    mtxInverse(mBaseMtx, inv);
    Mtx44 effect;
    mtxToMtx44(inv, effect);
    mRenderer->setEffectMtx(&effect);

    // J3DSys::drawInit / reinitGX defaults the materials do not override.
    GXSetCurrentMtx(GX_PNMTX0);
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_TRUE);
    GXSetFogRangeAdj(GX_FALSE, 0, nullptr);
    GXSetNumIndStages(0);

    mRenderer->draw(view);
}

} // namespace compat::j3d
