#pragma once
// =============================================================================
// PC_PORT (M9.5.3d) — TitleScene host reconstruction.
//
// The console scene holds TitleSequenceProduct + FileSelector + a 3D backdrop
// (FileSelectSky). M9.5.3d scope (docs/m9.5.3-plan.md): the real vendored
// TitleSequenceProduct over the FileSelectSky backdrop — M9.5.4 v8 draws the
// real CometNearOrbitSky model through compat/j3d (BmdRenderer), falling back
// to the v6/v7 host starfield when the archive is not present;
// FileSelector is M10. When the sequence ends (Decide) the scene
// parks and logs — the console loops back through GameSequenceProgress to the
// Title again, which without FileSelector would restart the intro forever.
// =============================================================================

#include "Game/Scene/Scene.hpp"

#include <memory>

class TitleSequenceProduct;
class SimpleLayout;

namespace compat::j3d {
    class TitleSky;
}

namespace compat::game {
    class FileSelectHost;
}

class TitleScene : public Scene {
public:
    TitleScene();
    ~TitleScene();

    void init();
    void update();
    void draw() const;
    void calcAnim();

    // Nerve handlers (public: NEW_NERVE stores member pointers).
    void exeTitle();
    void exeEnd();

private:
    TitleSequenceProduct* mTitle;
    /// PC_PORT: set once the sequence reached Dead; draw goes to a black frame
    /// afterwards (parked until the process exits — see the class comment).
    bool mEnded;
    /// PC_PORT (M9.5.4 v8): the real CometNearOrbitSky J3D backdrop
    /// (FileSelectSky stand-in, compat/j3d). Null when the archive is missing
    /// → the synthetic starfield is drawn instead.
    std::unique_ptr< compat::j3d::TitleSky > mSky;
    /// PC_PORT (M10): the post-Decide file-select screen
    /// (compat/game/FileSelectHost). It owns every layout of the family —
    /// FileSelect/FileInfo/BackButton/BrosButton/FileNumber all mount
    /// themselves through MR::connectToScene — plus the 3D planet field, the
    /// pointer and the save store, so the scene only calls init()/update()
    /// and places the 3D + cursor passes in draw(). Null until the title
    /// sequence decides (A+B).
    std::unique_ptr< compat::game::FileSelectHost > mFileHost;
};
