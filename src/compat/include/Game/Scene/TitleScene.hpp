#pragma once
// =============================================================================
// PC_PORT (M9.5.3d) — TitleScene host reconstruction.
//
// The console scene holds TitleSequenceProduct + FileSelector + a 3D backdrop
// (FileSelectSky). M9.5.3d scope (docs/m9.5.3-plan.md): the real vendored
// TitleSequenceProduct over a black backdrop; FileSelector is M10 and the 3D
// background is a later milestone. When the sequence ends (Decide) the scene
// parks and logs — the console loops back through GameSequenceProgress to the
// Title again, which without FileSelector would restart the intro forever.
// =============================================================================

#include "Game/Scene/Scene.hpp"

class TitleSequenceProduct;

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
};
