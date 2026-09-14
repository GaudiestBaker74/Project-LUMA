// =============================================================================
// compat/j3d — FileSelectField (see FileSelectField.h).
// =============================================================================

#include "compat/j3d/FileSelectField.h"

#include <revolution/gx.h>

#include <JSystem/JKernel/JKRArchive.hpp>
#include <JSystem/JKernel/JKRMemArchive.hpp>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "Game/Util/FileUtil.hpp"
#include "compat/game/UiAnchoring.h"
#include "compat/j3d/BmdRenderer.h"
#include "platform/Log/Log.h"

namespace compat::j3d {

namespace {

constexpr f32 kPi = 3.14159265358979323846f;

// FileSelectModel.cpp sScale: every file-select model is authored small and
// scaled by 30 on the actor.
constexpr f32 kModelScale = 30.0f;

// FileSelectItem::createNew: mPlanetMapObj->mScale.set(30.0f).
constexpr f32 kPlanetScale = 30.0f;

// The badge (the number model) floats this far above the item — measured on the
// retail capture: planet centre to badge centre is ~100 px at the far camera,
// i.e. ~1000 world units.
constexpr f32 kBadgeHeight = 1020.0f;

// The character head is drawn slightly in front of the planet so it reads as a
// separate object (the console gives FileSelectModel its own host matrix; on
// the capture the head overlaps the planet's lower-left third).
constexpr f32 kHeadOffsetZ = 520.0f;

// Item ids in FileSelectItem::createNew's sIndexOrder order.
constexpr f32 kItemSpread = 1.0f;

// Camera constants (FileSelectCameraController.cpp).
constexpr f32 kFarTarget[3] = {0.0f, 800.0f, 0.0f};
constexpr f32 kFarPoint[3] = {0.0f, 0.0f, 15000.0f};
constexpr f32 kNearTargetOffset[3] = {0.0f, 1100.0f, 0.0f};
constexpr f32 kNearPointOffset[3] = {0.0f, 0.0f, 4800.0f};
constexpr f32 kTitleFovy = 60.0f;
constexpr f32 kFarFovy = 40.0f;
constexpr f32 kNearFovy = 50.0f;
constexpr f32 kNearZ = 100.0f;
constexpr f32 kFarZ = 800000.0f;
constexpr f32 kUp[3] = {0.0f, 1.0f, 0.0f};

/// Fits the reconstructed layout without a rebuild: LUMA_FILESELECT_LAYOUT=0.95
/// shrinks the whole fan. Same escape-hatch idiom as TitleSky's scene scale.
f32 layoutScale() {
    static const f32 s = [] {
        const char* e = std::getenv("LUMA_FILESELECT_LAYOUT");
        return (e != nullptr) ? static_cast< f32 >(std::atof(e)) : 1.0f;
    }();
    return s;
}

// -----------------------------------------------------------------------------
// The reconstructed item placement (FileSelector::calcBasePos is not
// decompiled). See the header: measured from a retail capture through the far
// camera, then symmetrised (the left/right pairs came out within 20 units of
// each other on a 4960-unit spread, i.e. within the measurement error).
//   * 1/2 — the front pair, low and near the centre,
//   * 3/4 — the middle pair, wide and higher,
//   * 5/6 — the upper pair, higher still (the fan opens upwards),
// with a small -Z per row so the further pairs read as "behind" the front ones.
// -----------------------------------------------------------------------------
// Item base positions. FileSelector::calcBasePos has NO decompiled body (the
// petari file marks it with a comment only), so the table was RECONSTRUCTED
// from the retail screen and then solved numerically:
//
//   1. the six badge numerals of docs/images/fileselect/1_seleccion_save.png (1920x1080) were
//      located by flood-filling their gold ink — the clusters come out in three
//      symmetric pairs in design units:
//          1/2 (-101.1, -4.1) / (100.6, -5.3)
//          3/4 (-207.9, 51.2) / (207.1, 50.6)
//          5/6 ( -82.0, 80.8) / ( 80.3, 79.4)
//   2. each pair was inverted through the far camera of
//      FileSelectCameraController (pos (0,0,15000), target (0,800,0), fovy 40,
//      228 design units per tan(20 deg) at the projection plane) to the world
//      position of the BADGE;
//   3. the item position is the badge minus kBadgeHeight, because that is the
//      relation calcBadgeWorldPos implements (FileSelectNumber is placed from
//      the item's 3D position).
//
// The result reproduces the reference badge positions to a couple of units.
const FileSelectItemPlacement kPlacements[FileSelectField::kItemNum] = {
    {1, -2417.0f * kItemSpread, -333.0f * kItemSpread, 0.0f},
    {2, 2417.0f * kItemSpread, -333.0f * kItemSpread, 0.0f},
    {3, -5197.0f * kItemSpread, 1089.0f * kItemSpread, -600.0f},
    {4, 5197.0f * kItemSpread, 1089.0f * kItemSpread, -600.0f},
    {5, -2116.0f * kItemSpread, 1935.0f * kItemSpread, -1200.0f},
    {6, 2116.0f * kItemSpread, 1935.0f * kItemSpread, -1200.0f},
};

// FileSelectItem.cpp sFellowModel — index = FileSelectCharacter.
const char* const kFellowModels[] = {"FileSelectDataMario", "FileSelectDataLuigi", "FileSelectDataYoshi",
                                     "FileSelectDataKinopio", "FileSelectDataPeach"};

const char* const kPlanetModel = "FileSelectDataPlanet";

std::string objectArcPath(const char* modelName) {
    std::string s = "/ObjectData/";
    s += modelName;
    s += ".arc";
    return s;
}

void mtxTranslateScaleRotY(Mtx out, f32 x, f32 y, f32 z, f32 scale, f32 rotY) {
    const f32 c = std::cos(rotY);
    const f32 s = std::sin(rotY);

    out[0][0] = c * scale;
    out[0][1] = 0.0f;
    out[0][2] = -s * scale;
    out[0][3] = x;

    out[1][0] = 0.0f;
    out[1][1] = scale;
    out[1][2] = 0.0f;
    out[1][3] = y;

    out[2][0] = s * scale;
    out[2][1] = 0.0f;
    out[2][2] = c * scale;
    out[2][3] = z;
}

}  // namespace

FileSelectField::FileSelectField() {
    for (int i = 0; i < kItemNum; ++i) {
        mItems[i].character = static_cast< FileSelectCharacter >(i % 5);
    }
}

FileSelectField::~FileSelectField() = default;

const FileSelectItemPlacement& FileSelectField::placement(int item) {
    const int idx = (item < 0) ? 0 : (item >= kItemNum ? kItemNum - 1 : item);

    return kPlacements[idx];
}

void FileSelectField::calcItemWorldPos(int item, f32 zShift, f32* outX, f32* outY, f32* outZ) {
    const FileSelectItemPlacement& p = placement(item);
    const f32 s = layoutScale();

    if (outX != nullptr) {
        *outX = p.x * s;
    }
    if (outY != nullptr) {
        *outY = p.y * s;
    }
    if (outZ != nullptr) {
        *outZ = p.z + zShift;
    }
}

void FileSelectField::calcBadgeWorldPos(int item, f32 zShift, f32* outX, f32* outY, f32* outZ) {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
    calcItemWorldPos(item, zShift, &x, &y, &z);

    if (outX != nullptr) {
        *outX = x;
    }
    if (outY != nullptr) {
        *outY = y + kBadgeHeight * layoutScale();
    }
    if (outZ != nullptr) {
        *outZ = z;
    }
}

FileSelectCamera FileSelectField::cameraFar() {
    FileSelectCamera cam;
    cam.pos[0] = kFarPoint[0];
    cam.pos[1] = kFarPoint[1];
    cam.pos[2] = kFarPoint[2];
    cam.target[0] = kFarTarget[0];
    cam.target[1] = kFarTarget[1];
    cam.target[2] = kFarTarget[2];
    cam.up[0] = kUp[0];
    cam.up[1] = kUp[1];
    cam.up[2] = kUp[2];
    cam.fovy = kFarFovy;

    return cam;
}

FileSelectCamera FileSelectField::cameraTitle() {
    // FileSelectCameraController::Title (lines 79-80 of the decomp): the whole
    // far state lifted by 15000 — the TARGET moves with cFarTarget.y and the
    // camera with cFarPoint.y, which is what makes the title state look up at
    // the fan while it flies in.
    //   mWPoint   = (cFarTarget.x, cFarTarget.y + 15000, cFarTarget.z)
    //   mPosition = (cFarPoint.x,  cFarPoint.y  + 15000, cFarPoint.z)
    FileSelectCamera cam = cameraFar();
    cam.target[1] = kFarTarget[1] + 15000.0f;
    cam.pos[1] = kFarPoint[1] + 15000.0f;
    cam.fovy = kTitleFovy;

    return cam;
}

FileSelectCamera FileSelectField::cameraNear(f32 itemX, f32 itemY, f32 itemZ) {
    FileSelectCamera cam;
    cam.target[0] = itemX;                      // cNearPointOffset.x == 0
    cam.target[1] = itemY + kNearTargetOffset[1];
    cam.target[2] = itemZ;
    cam.pos[0] = cam.target[0] + kNearPointOffset[0];
    cam.pos[1] = cam.target[1] + kNearPointOffset[1];
    cam.pos[2] = cam.target[2] + kNearPointOffset[2];
    cam.up[0] = kUp[0];
    cam.up[1] = kUp[1];
    cam.up[2] = kUp[2];
    cam.fovy = kNearFovy;

    return cam;
}

FileSelectCamera FileSelectField::blendCamera(const FileSelectCamera& from, const FileSelectCamera& to, f32 t) {
    // FileSelectCameraController::exeMoveToFarPoint: squaredTime = step / 60,
    // squaredTime *= squaredTime, then param += (target - param) * squaredTime.
    // The move itself is 60 frames, so the caller passes step/60 here and the
    // squaring happens once (it is the *eased* fraction, not the raw one).
    if (t < 0.0f) {
        t = 0.0f;
    }
    if (t > 1.0f) {
        t = 1.0f;
    }

    const f32 e = t * t;

    FileSelectCamera out = from;

    for (int i = 0; i < 3; ++i) {
        out.pos[i] = from.pos[i] + (to.pos[i] - from.pos[i]) * e;
        out.target[i] = from.target[i] + (to.target[i] - from.target[i]) * e;
        out.up[i] = from.up[i] + (to.up[i] - from.up[i]) * e;
    }

    out.fovy = from.fovy + (to.fovy - from.fovy) * e;

    return out;
}

FileSelectItemScreen FileSelectField::project(const FileSelectCamera& camera, f32 worldX, f32 worldY, f32 worldZ,
                                              f32 fbWidth, f32 fbHeight) {
    FileSelectItemScreen out;

    if (fbWidth <= 0.0f || fbHeight <= 0.0f) {
        return out;
    }

    // Forward / right / up basis of the look-at.
    f32 f[3] = {camera.target[0] - camera.pos[0], camera.target[1] - camera.pos[1], camera.target[2] - camera.pos[2]};
    f32 flen = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);

    if (flen <= 0.0f) {
        return out;
    }

    f[0] /= flen;
    f[1] /= flen;
    f[2] /= flen;

    // right = normalize(cross(forward, up))
    f32 r[3] = {f[1] * camera.up[2] - f[2] * camera.up[1], f[2] * camera.up[0] - f[0] * camera.up[2],
                f[0] * camera.up[1] - f[1] * camera.up[0]};
    const f32 rlen = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);

    if (rlen <= 0.0f) {
        return out;
    }

    r[0] /= rlen;
    r[1] /= rlen;
    r[2] /= rlen;

    // up' = cross(right, forward)
    const f32 u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};

    const f32 dx = worldX - camera.pos[0];
    const f32 dy = worldY - camera.pos[1];
    const f32 dz = worldZ - camera.pos[2];

    const f32 viewX = dx * r[0] + dy * r[1] + dz * r[2];
    const f32 viewY = dx * u[0] + dy * u[1] + dz * u[2];
    const f32 viewZ = dx * f[0] + dy * f[1] + dz * f[2];

    if (viewZ <= 1.0f) {
        return out;  // behind (or on) the camera plane
    }

    // GX perspective: vertical fovy, aspect from the framebuffer.
    const f32 aspect = fbWidth / fbHeight;
    const f32 tanHalf = std::tan(camera.fovy * 0.5f * (kPi / 180.0f));

    const f32 ndcX = viewX / (viewZ * tanHalf * aspect);
    const f32 ndcY = viewY / (viewZ * tanHalf);

    out.visible = true;
    out.x = (ndcX * 0.5f + 0.5f) * fbWidth;
    out.y = (0.5f - ndcY * 0.5f) * fbHeight;
    out.depth = viewZ;

    // Apparent size of the pointing cylinder: FileSelectItem::initStarPointer
    // Target(this, 1000.0f, TVec3f(0, 900, 0)) — radius 1000 world units.
    const f32 pxPerUnit = (fbHeight * 0.5f) / (viewZ * tanHalf);
    out.radiusPx = 1000.0f * pxPerUnit;

    return out;
}

void FileSelectField::calcViewMtx(const FileSelectCamera& camera, Mtx out) {
    // Same idiom as compat/j3d/TitleSky (the game's CameraContext look-at):
    // view = look-at(camera.pos, camera.up, camera.target).
    f32 f[3] = {camera.target[0] - camera.pos[0], camera.target[1] - camera.pos[1], camera.target[2] - camera.pos[2]};
    const f32 flen = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);

    if (flen > 0.0f) {
        f[0] /= flen;
        f[1] /= flen;
        f[2] /= flen;
    }

    f32 r[3] = {f[1] * camera.up[2] - f[2] * camera.up[1], f[2] * camera.up[0] - f[0] * camera.up[2],
                f[0] * camera.up[1] - f[1] * camera.up[0]};
    const f32 rlen = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);

    if (rlen > 0.0f) {
        r[0] /= rlen;
        r[1] /= rlen;
        r[2] /= rlen;
    }

    const f32 u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};

    // GX view matrix: rows = right / up / -forward, translation = -(row . pos).
    out[0][0] = r[0];
    out[0][1] = r[1];
    out[0][2] = r[2];
    out[0][3] = -(r[0] * camera.pos[0] + r[1] * camera.pos[1] + r[2] * camera.pos[2]);

    out[1][0] = u[0];
    out[1][1] = u[1];
    out[1][2] = u[2];
    out[1][3] = -(u[0] * camera.pos[0] + u[1] * camera.pos[1] + u[2] * camera.pos[2]);

    out[2][0] = -f[0];
    out[2][1] = -f[1];
    out[2][2] = -f[2];
    out[2][3] = f[0] * camera.pos[0] + f[1] * camera.pos[1] + f[2] * camera.pos[2];
}

namespace {

struct ArcFile {
    std::string name;
    std::vector< u8 > bytes;
};

bool endsWithIgnoreCase(const char* pStr, const char* pSuffix) {
    const size_t n = std::strlen(pStr);
    const size_t m = std::strlen(pSuffix);

    if (m > n) {
        return false;
    }

    for (size_t i = 0; i < m; ++i) {
        char a = pStr[n - m + i];

        if (a >= 'A' && a <= 'Z') {
            a = static_cast< char >(a - 'A' + 'a');
        }

        if (a != pSuffix[i]) {
            return false;
        }
    }

    return true;
}

/// Copies every file of the archive matching one of the extensions (the same
/// walk compat/j3d/TitleSky uses for the sky dome).
void collectFiles(JKRArchive* pArc, const char* const* exts, std::vector< ArcFile >& out) {
    for (u32 i = 0;; ++i) {
        JKRArchive::SDirEntry dir;

        if (!pArc->getDirEntry(&dir, i)) {
            break;
        }

        if (((dir.mFileFlag >> JKRArchive::FILE_FLAG_FOLDER_SHIFT) & 1) != 0 || dir.mName == nullptr) {
            continue;
        }

        bool match = false;

        for (const char* const* e = exts; *e != nullptr; ++e) {
            if (endsWithIgnoreCase(dir.mName, *e)) {
                match = true;
                break;
            }
        }

        if (!match) {
            continue;
        }

        void* pRes = pArc->getIdxResource(i);

        if (pRes == nullptr) {
            continue;
        }

        const s32 size = pArc->getResSize(pRes);

        if (size <= 0) {
            continue;
        }

        ArcFile file;
        file.name = dir.mName;
        file.bytes.assign(static_cast< const u8* >(pRes), static_cast< const u8* >(pRes) + size);
        out.push_back(std::move(file));
    }
}

std::string baseName(const char* pName) {
    std::string s(pName);
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

/// Picks the file named after `modelName`, else the first one (the
/// ModelManager rule MR::initModelManagerWithAnm follows on the console).
size_t pickNamed(const std::vector< ArcFile >& files, const char* modelName, size_t fallback) {
    for (size_t i = 0; i < files.size(); ++i) {
        if (baseName(files[i].name.c_str()) == modelName) {
            return i;
        }
    }

    return fallback;
}

}  // namespace

bool FileSelectField::loadModel(const char* arcPath, const char* modelName, std::unique_ptr< BmdRenderer >* out) {
    // The mount path the title sky uses: MR::mountArchive (JKRMemArchive over
    // the compat DVD layer) + the archive's own copy of the model, parsed by
    // the host BMD reader (compat/j3d/BmdModel). An object archive holds one
    // model plus its BTK/BCK animations.
    JKRMemArchive* pArchive = MR::mountArchive(arcPath, nullptr);

    if (pArchive == nullptr) {
        mMissing.push_back(arcPath);
        return false;
    }

    static const char* const kModelExts[] = {".bdl", ".bmd", nullptr};
    static const char* const kBtkExts[] = {".btk", nullptr};
    static const char* const kBckExts[] = {".bck", nullptr};

    std::vector< ArcFile > models;
    std::vector< ArcFile > btks;
    std::vector< ArcFile > bcks;
    collectFiles(pArchive, kModelExts, models);
    collectFiles(pArchive, kBtkExts, btks);
    collectFiles(pArchive, kBckExts, bcks);

    if (models.empty()) {
        PL_LOG_WARN("fileselect", "FileSelectField: '%s' holds no bmd/bdl", arcPath);
        mMissing.push_back(arcPath);
        return false;
    }

    const size_t pick = pickNamed(models, modelName, 0);

    std::unique_ptr< BmdRenderer > renderer(new BmdRenderer());
    std::string error;

    if (!renderer->init(std::move(models[pick].bytes), &error)) {
        PL_LOG_WARN("fileselect", "FileSelectField: '%s' (%s) failed to parse: %s", arcPath,
                    models[pick].name.c_str(), error.c_str());
        mMissing.push_back(arcPath);
        return false;
    }

    // The object's animations (FileSelectModel::initModelManagerWithAnm picks
    // every .bck/.btk of the archive): the blink of the heads, the planet's
    // idle motion.
    if (!bcks.empty()) {
        std::string err;
        const size_t idx = pickNamed(bcks, modelName, 0);

        if (!renderer->attachBck(bcks[idx].bytes, &err)) {
            PL_LOG_WARN("fileselect", "FileSelectField: '%s' bck not attached: %s", bcks[idx].name.c_str(),
                        err.c_str());
        }
    }

    if (!btks.empty()) {
        std::string err;
        const size_t idx = pickNamed(btks, modelName, 0);

        if (!renderer->attachBtk(btks[idx].bytes, &err)) {
            PL_LOG_WARN("fileselect", "FileSelectField: '%s' btk not attached: %s", btks[idx].name.c_str(),
                        err.c_str());
        }
    }

    // The planets and the heads are closed surfaces drawn at scale 30; keeping
    // both faces visible is what the console does for skinned/billboard parts
    // and is visually identical for the opaque ones.
    renderer->setCullModeOverride(GX_CULL_NONE);

    *out = std::move(renderer);

    PL_LOG_INFO("fileselect", "FileSelectField: '%s' -> model '%s' (%zu verts, %zu materials, %zu bck)", arcPath,
                baseName(models[pick].name.c_str()).c_str(),
                static_cast< unsigned >(renderer->model().vertexCount),
                renderer->model().materials.size(), bcks.size());

    return true;
}

bool FileSelectField::init() {
    if (!loadModel(objectArcPath(kPlanetModel).c_str(), kPlanetModel, &mPlanet)) {
        PL_LOG_WARN("fileselect",
                    "FileSelectField: %s is missing — the planet models cannot be drawn (the layout and the "
                    "cursor still work)",
                    objectArcPath(kPlanetModel).c_str());
        return false;
    }

    mLoaded = true;
    u32 heads = 0;

    for (int i = 0; i < static_cast< int >(FileSelectCharacter::Count); ++i) {
        const char* pModel = kFellowModels[i];

        if (loadModel(objectArcPath(pModel).c_str(), pModel, &mHeads[i])) {
            ++heads;
        }
    }

    mHeadsLoaded = heads > 0;

    PL_LOG_INFO("fileselect", "FileSelectField: 6 items ready (planet + %u/5 character heads)", heads);

    return true;
}

void FileSelectField::setScale(int item, f32 scale) {
    if (item < 0 || item >= kItemNum) {
        return;
    }

    mItems[item].scaleTarget = scale;
}

f32 FileSelectField::scale(int item) const {
    return (item >= 0 && item < kItemNum) ? mItems[item].scale : 1.0f;
}

void FileSelectField::setZShift(f32 dz) {
    mZShift = dz;
}

void FileSelectField::setHasSave(int item, bool hasSave) {
    if (item < 0 || item >= kItemNum) {
        return;
    }

    mItems[item].hasSave = hasSave;
}

bool FileSelectField::hasSave(int item) const {
    return (item >= 0 && item < kItemNum) ? mItems[item].hasSave : false;
}

void FileSelectField::setCharacter(int item, FileSelectCharacter character) {
    if (item < 0 || item >= kItemNum) {
        return;
    }

    mItems[item].character = character;
}

FileSelectCharacter FileSelectField::character(int item) const {
    return (item >= 0 && item < kItemNum) ? mItems[item].character : FileSelectCharacter::Mario;
}

void FileSelectField::setAppearRate(f32 rate) {
    mAppear = rate < 0.0f ? 0.0f : (rate > 1.0f ? 1.0f : rate);
}

void FileSelectField::setVisible(bool visible) {
    mVisible = visible;
}

void FileSelectField::update() {
    // 60 Hz game frames.
    ++mFrame;
    mTime = static_cast< f32 >(mFrame) / 60.0f;

    for (int i = 0; i < kItemNum; ++i) {
        Item& item = mItems[i];

        // ScaleController::exeToBig / exeToSmall: 30 frames of exponential
        // approach to 1.2 (pointed) or 1.0 (idle).
        const f32 step = 1.0f / 30.0f;
        item.scale += (item.scaleTarget - item.scale) * step;

        // Idle motion: a slow spin and a small bob, so the planets read as
        // alive — the console animates the item's rotation toward the pointer
        // and bobs the ModelObj (FileSelectItem::updateRotate).
        item.spin += 0.0025f;
        item.bob = std::sin(mTime * 1.1f + static_cast< f32 >(i) * 1.3f) * 40.0f;
    }

    // Appear animation (the screen mounts): the items grow into place.
    if (mAppear < 1.0f) {
        mAppear += 1.0f / 45.0f;

        if (mAppear > 1.0f) {
            mAppear = 1.0f;
        }
    }
}

void FileSelectField::drawOne(const Mtx view, int item, const std::unique_ptr< BmdRenderer >& renderer, f32 scale,
                              f32 spin, f32 worldX, f32 worldY, f32 worldZ) {
    if (!renderer || !renderer->loaded()) {
        return;
    }

    Mtx base;
    mtxTranslateScaleRotY(base, worldX, worldY, worldZ, kModelScale, spin);

    renderer->setBaseMtx(base);
    renderer->setBaseScale(scale * mAppear, scale * mAppear, scale * mAppear);
    renderer->draw(view);

    (void)item;
}

void FileSelectField::draw(const Mtx view) {
    mLastDrawn = 0;

    if (!mVisible || !mLoaded) {
        return;
    }

    for (int i = 0; i < kItemNum; ++i) {
        const Item& item = mItems[i];

        f32 x = 0.0f;
        f32 y = 0.0f;
        f32 z = 0.0f;
        calcItemWorldPos(i, mZShift, &x, &y, &z);
        y += item.bob;

        // The character head, for slots that hold a save: FileSelectItem draws
        // the fellow model in front of the planet (createFellows/createNew).
        if (item.hasSave && mHeadsLoaded) {
            const int ch = static_cast< int >(item.character);

            if (ch >= 0 && ch < static_cast< int >(FileSelectCharacter::Count) && mHeads[ch]) {
                drawOne(view, i, mHeads[ch], item.scale * 0.92f, item.spin * 0.4f, x, y, z + kHeadOffsetZ);
                mLastDrawn++;
            }
        }

        // The planet itself: the "new file" planet (FileSelectDataPlanet) is the
        // one always drawn — saved slots draw the head over it.
        drawOne(view, i, mPlanet, item.scale, item.spin, x, y, z);
        mLastDrawn++;
    }
}

}  // namespace compat::j3d
