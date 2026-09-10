#pragma once
// =============================================================================
// compat/j3d — BCK (J3D joint keyframe animation, ANK1) reader + evaluator
// (M9.5.4 v8).
//
// Mirrors J3DAnmTransformKey (J3DAnimation.cpp) + J3DAnmKeyLoader_v15::
// setAnmTransform: one entry per joint with nine key tracks (scale/rotation/
// translation × X/Y/Z). Rotation keys are s16 units shifted left by the
// file's decimal shift. While attached, the evaluated transform replaces the
// joint's JNT1 transform before the hierarchy matrices are built
// (J3DMtxCalcAnmBase).
// =============================================================================

#include <revolution/types.h>

#include <cstddef>
#include <string>
#include <vector>

#include "compat/j3d/J3DMathCompat.h"

namespace compat::j3d {

struct BckJointTracks {
    KeyTrack scale[3];
    KeyTrack rotation[3];
    KeyTrack translation[3];
};

class BckAnim {
public:
    /// Parses a J3D1bck1 file. Returns false with `error` set when malformed.
    bool load(const u8* data, size_t size, std::string* error = nullptr);

    /// Evaluates joint `i` at `frame` (J3DAnmTransformKey::getTransform).
    void evaluate(size_t i, f32 frame, JointTransform& out) const;

    u8 loopMode = 2;
    u8 rotDecShift = 0;
    u16 duration = 0;
    std::vector<BckJointTracks> joints;

private:
    std::vector<f32> mScaleData;
    std::vector<s16> mRotData;
    std::vector<f32> mTransData;
};

} // namespace compat::j3d
