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
#include "compat/j3d/BmdRenderer.h"
#include "platform/Log/Log.h"

namespace compat::j3d {

namespace {

constexpr f32 kPi = 3.14159265358979323846f;
constexpr f32 kAngleIncY = 0.001f;   // FileSelectSky.cpp cAngleIncY
constexpr f32 kCycleX = 3000.0f;     // FileSelectSky.cpp cCycleX
constexpr f32 kScale = 0.8f;
// FileSelectCameraController::exeTitle: cFarTarget/cFarPoint + 15000 on Y.
constexpr f32 kCamPos[3] = {0.0f, 15000.0f, 15000.0f};
constexpr f32 kCamTarget[3] = {0.0f, 15800.0f, 0.0f};
constexpr f32 kCamUp[3] = {0.0f, 1.0f, 0.0f};
constexpr f32 kTitleFovy = 60.0f;
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

    renderer->setBaseScale(kScale, kScale, kScale);
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
    PL_LOG_INFO("j3d", "TitleSky: '%s' ready (%s, %zu btk, %zu bck)", archivePath, mModelName.c_str(), btks.size(),
                bcks.size());
    return true;
}

void TitleSky::update() {
    if (!mLoaded || !mRenderer) {
        return;
    }
    // FileSelectSky::exeWait (the nerve step counts frames since appear).
    calcBaseMtx(mAngleX, mAngleY, mBaseMtx);
    mAngleX = calcAngleX(mStep);
    mAngleY += kAngleIncY;
    if (mAngleY > 2.0f * kPi) {
        mAngleY -= 2.0f * kPi;
    }
    ++mStep;
    // LiveActor::calcAnim: advance the model's animations, then
    // ProjmapEffectMtxSetter::updateMtxUseBaseMtx.
    mRenderer->update();
}

void TitleSky::draw() {
    if (!mLoaded || !mRenderer) {
        return;
    }
    // Camera: rotation part of the title camera (Sky actors sit on the camera
    // position, so the translation cancels out).
    Mtx view;
    mtxLookAt(view, kCamPos, kCamUp, kCamTarget);
    mtxZeroTranslation(view);

    Mtx44 proj;
    const f32 aspect = static_cast<f32>(MR::getScreenWidth()) / static_cast<f32>(MR::getScreenHeight());
    C_MTXPerspective(proj, kTitleFovy, aspect, kNearZ, kFarZ);
    GXSetProjection(proj, GX_PERSPECTIVE);

    // Base matrix + effect matrix (ProjmapEffectMtxSetter: inverse(base)).
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
