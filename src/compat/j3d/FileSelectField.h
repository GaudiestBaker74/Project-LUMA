#pragma once
// =============================================================================
// compat/j3d — FileSelectField (PC_PORT M10.1): the 3D half of the file-select
// screen — the six planets, the character heads that float in front of the
// saved ones, and the FileSelectCameraController camera the screen looks
// through.
//
// -----------------------------------------------------------------------------
// Why this exists
// -----------------------------------------------------------------------------
// On the console these are LiveActors: FileSelectItem (a planet/character
// actor, 6 of them) grouped by FileSelector, each one a FileSelectModel (a J3D
// model through the ModelManager) placed by FileSelector::calcBasePos and lit
// by the actor lights, with FileSelectCameraController flying the camera
// between the "title", "far" and "near" points. None of that machinery is
// ported (ModelManager/J3D/LiveActor draw buffers), so the M10 stand-in mounted
// only FileSelect.arc and the screen had NO planets at all — the M10.1 report
// ("tampoco salen los planetas para seleccionar").
//
// This module reproduces exactly what the actors contribute visually, on top of
// the host J3D path that already draws the title sky (compat/j3d/BmdRenderer):
//   * the planet: /ObjectData/FileSelectDataPlanet.arc -> "FileSelectDataPlanet",
//     scale 30 (FileSelectItem::createNew),
//   * the character head: /ObjectData/FileSelectData{Mario,Luigi,Yoshi,
//     Kinopio,Peach}.arc (FileSelectItem.cpp sFellowModel), scale 30
//     (FileSelectModel.cpp sScale) for the slots that hold a save,
//   * the item placement: FileSelector::calcBasePos is NOT decompiled in petari
//     (only a comment marks it), so the base positions below are RECONSTRUCTED
//     from the retail screen: the badge positions of a 1920x1080 capture were
//     measured (gold-numeral clusters) and inverted through the far camera of
//     FileSelectCameraController::exeFarPoint. They come out perfectly
//     symmetric — pairs at |x| = 2400 / 4960 / 1947 with matching y — which is
//     what a hand-authored arc looks like, so the table is trustworthy to a few
//     percent (LUMA_FILESELECT_LAYOUT scales it without touching the code).
//   * the camera: the real constants of FileSelectCameraController.cpp —
//     far point (0, 0, 15000) -> target (0, 800, 0), fovy 40; near point
//     itemPos + (0, 1100, 0) with the camera 4800 behind it, fovy 50; title
//     point (0, 15000, 15000) -> (0, 15800, 0), fovy 60; and the 60-frame
//     squared-time interpolation of exeMoveTo*Point.
// =============================================================================

#include <revolution/types.h>
#include <revolution/mtx.h>

#include <memory>
#include <string>
#include <vector>

namespace compat::j3d {

class BmdRenderer;

/// One of the six items on the file-select screen.
struct FileSelectItemPlacement {
    s32 number;      // the badge number, 1..6 (FileSelector sIndexOrder)
    f32 x, y, z;     // world base position (FileSelector::calcBasePos)
};

/// Camera state (FileSelectCameraController): a look-at plus a vertical fovy.
struct FileSelectCamera {
    f32 pos[3];
    f32 target[3];
    f32 up[3];
    f32 fovy;
};

/// Where an item lands on screen (pixels, top-left origin) and how big it is.
struct FileSelectItemScreen {
    bool visible = false;
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 radiusPx = 0.0f;   // apparent radius of the pointing cylinder
    f32 depth = 1.0f;      // view depth in world units (for scale/priority)
};

/// Which character a saved slot shows (FileSelectItem::sFellowModel order).
enum class FileSelectCharacter : s32 {
    Mario = 0,
    Luigi = 1,
    Yoshi = 2,
    Kinopio = 3,
    Peach = 4,
    Count,
};

class FileSelectField {
public:
    static constexpr int kItemNum = 6;

    FileSelectField();
    ~FileSelectField();

    /// Mounts the model archives. Returns false when the planet archive is
    /// missing (asset-less runs): the field then draws nothing and every
    /// geometry helper below keeps working (the hit test stays consistent).
    bool init();
    bool loaded() const { return mLoaded; }
    /// True when the per-character head archives loaded (saves show a head).
    bool headsLoaded() const { return mHeadsLoaded; }
    /// Names of the archives that failed to load (diagnostics/tests).
    const std::vector<std::string>& missingArchives() const { return mMissing; }

    /// One 60 Hz game frame: item scale easing, the floating bob, head blink.
    void update();

    /// Draws the items. `view` is the camera matrix (GX convention); the caller
    /// has already set the projection with GXSetProjection.
    void draw(const Mtx view);

    // --- item state (driven by the file-select screen) ----------------------
    /// ScaleController: 1.0 idle, 1.2 while pointed (exeToBig/e xeToSmall).
    void setScale(int item, f32 scale);
    f32 scale(int item) const;
    /// FileSelector::calcBasePos(dz): the items are pushed away on select.
    void setZShift(f32 dz);
    f32 zShift() const { return mZShift; }
    void setHasSave(int item, bool hasSave);
    bool hasSave(int item) const;
    void setCharacter(int item, FileSelectCharacter character);
    FileSelectCharacter character(int item) const;
    /// Appearance animation: items fade/scale in when the screen mounts.
    void setAppearRate(f32 rate);  // 0..1
    f32 appearRate() const { return mAppear; }
    void setVisible(bool visible);
    bool visible() const { return mVisible; }

    // --- geometry / camera (pure, unit-tested) ------------------------------
    static const FileSelectItemPlacement& placement(int item);
    /// World position of an item including the push-away shift.
    static void calcItemWorldPos(int item, f32 zShift, f32* outX, f32* outY, f32* outZ);
    /// Where the badge (the number floating above the planet) sits.
    static void calcBadgeWorldPos(int item, f32 zShift, f32* outX, f32* outY, f32* outZ);

    static FileSelectCamera cameraFar();
    static FileSelectCamera cameraTitle();
    static FileSelectCamera cameraNear(f32 itemX, f32 itemY, f32 itemZ);
    /// fileSelectCamLerp: the 60-frame squared-time move of exeMoveTo*Point.
    static FileSelectCamera blendCamera(const FileSelectCamera& from, const FileSelectCamera& to, f32 t);
    /// World -> screen pixels for a framebuffer of `fbWidth` x `fbHeight`.
    static FileSelectItemScreen project(const FileSelectCamera& camera, f32 worldX, f32 worldY, f32 worldZ,
                                        f32 fbWidth, f32 fbHeight);
    /// Looks at a world point and produces the GX view matrix.
    static void calcViewMtx(const FileSelectCamera& camera, Mtx out);

    /// Number of items drawn by the last draw() (diagnostics/tests).
    u32 lastDrawnItems() const { return mLastDrawn; }

private:
    struct Item {
        bool hasSave = false;
        FileSelectCharacter character = FileSelectCharacter::Mario;
        f32 scale = 1.0f;       // current (animated)
        f32 scaleTarget = 1.0f;
        f32 spin = 0.0f;        // the planet's slow rotation
        f32 bob = 0.0f;
    };

    bool loadModel(const char* arcPath, const char* modelName, std::unique_ptr<BmdRenderer>* out);
    void drawOne(const Mtx view, int item, const std::unique_ptr<BmdRenderer>& renderer, f32 scale, f32 spin,
                 f32 worldX, f32 worldY, f32 worldZ);

    std::unique_ptr<BmdRenderer> mPlanet;
    std::unique_ptr<BmdRenderer> mHeads[static_cast<int>(FileSelectCharacter::Count)];

    Item mItems[kItemNum];
    bool mLoaded = false;
    bool mHeadsLoaded = false;
    bool mVisible = true;
    f32 mZShift = 0.0f;
    f32 mAppear = 1.0f;
    f32 mTime = 0.0f;
    u32 mLastDrawn = 0;
    u32 mFrame = 0;
    std::vector<std::string> mMissing;
};

}  // namespace compat::j3d
