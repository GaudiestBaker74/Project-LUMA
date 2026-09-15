#pragma once
// =============================================================================
// compat/j3d — TitleSky: host stand-in for the FileSelectSky actor
// (M9.5.4 v8).
//
// The console title screen draws the "CometNearOrbitSky" J3D model (blue
// cloud dome + cyan sea band) behind the logo layouts. FileSelectSky
// (Game/Map/FileSelectSky.cpp) is a LiveActor: it needs the ModelManager,
// the draw-buffer holder, the effect keeper and the nerve machinery, none of
// which are ported. This class reproduces just what the actor does:
//   * mounts /ObjectData/CometNearOrbitSky.arc and loads the bmd/bdl + the
//     "CometNearOrbitSky" btk/bck (MR::initModelManagerWithAnm +
//     startBck/startBtk),
//   * scale 0.8, base matrix = inverse(rotY(angleY) * rotX(angleX)) with
//     angleY += 0.001 rad/frame and angleX = (1 - cos(8·step·π/3000)) ·
//     1.5 · π/4 (exeWait, JMACosShort on s16 angle units),
//   * ProjmapEffectMtxSetter::updateMtxUseBaseMtx → effect matrix =
//     inverse(base matrix),
//   * the title camera of FileSelectCameraController::exeTitle (position
//     (0, 15000, 15000) looking at (0, 15800, 0), up +Y, fovy 60, aspect from
//     the framebuffer) with CameraContext's near/far 100/800000. The base
//     matrix above is rotation-only, so the dome sits at the WORLD ORIGIN and
//     the camera's translation is part of the placement (do not cancel it).
//
// draw() sets the whole GX state it needs and leaves the layout pass to
// restore its own (TitleScene::draw calls MR::drawInitFor2DModel afterwards).
// =============================================================================

#include <revolution/types.h>
#include <revolution/mtx.h>

#include <memory>
#include <string>

namespace compat::j3d {

class BmdRenderer;

class TitleSky {
public:
    TitleSky();
    ~TitleSky();

    /// Mounts the archive and loads the model/animations. Returns false (and
    /// logs once) when the archive is missing; draw() is then a no-op.
    bool init(const char* archivePath = "/ObjectData/CometNearOrbitSky.arc");
    bool loaded() const { return mLoaded; }

    /// One game frame (60 Hz): FileSelectSky::exeWait + calcAnim.
    void update();
    /// Draws the dome for the current frame (GX state fully set here).
    void draw();

    /// PC_PORT (fileselect sky framing): while the fileselect screen is
    /// mounted the dome is drawn with a higher framing. The console
    /// fileselect background is a pure starfield — the bright sea band sits
    /// below the bottom edge of the frame in both the far and the near
    /// camera states — while the title framing puts that band mid-screen.
    /// TitleScene toggles this from mEnded && mFileHost; the framing blends
    /// at 1/45 per frame (the FileSelectHost appear animation duration) so
    /// the band slides out of the frame as the planets arrange themselves.
    void setFileSelectActive(bool active);
    /// Diagnostics/tests: current framing blend, 0 = title framing,
    /// 1 = fileselect (starfield) framing.
    f32 fileSelectBlend() const { return mFsBlend; }

    /// Diagnostics/tests.
    f32 angleX() const { return mAngleX; }
    f32 angleY() const { return mAngleY; }
    const BmdRenderer* renderer() const { return mRenderer.get(); }
    BmdRenderer* renderer() { return mRenderer.get(); }
    /// Computes the base matrix for the given step/angles (FileSelectSky::exeWait).
    static void calcBaseMtx(f32 angleX, f32 angleY, Mtx out);
    static f32 calcAngleX(u32 step);

private:
    std::unique_ptr<BmdRenderer> mRenderer;
    bool mLoaded = false;
    u32 mStep = 0;
    f32 mAngleX = 0.0f;
    f32 mAngleY = 0.0f;
    Mtx mBaseMtx;
    std::string mModelName;
    // PC_PORT (fileselect sky framing): blend between the title framing
    // (0) and the fileselect starfield framing (1), advanced in update().
    f32 mFsBlend = 0.0f;
    f32 mFsTarget = 0.0f;
};

} // namespace compat::j3d
