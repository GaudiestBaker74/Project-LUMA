#pragma once
// =============================================================================
// compat/j3d — BTK (J3D texture SRT keyframe animation, TTK1) reader +
// evaluator (M9.5.4 v8).
//
// Mirrors J3DAnmTextureSRTKey (J3DAnimation.cpp) + the BTK loader in
// J3DAnmLoader.cpp: every entry animates one texture matrix (material name +
// texMtx slot) with three key tracks S/T/Q; scale X/Y come from S/T, the
// rotation from Q, translation X/Y from S/T. Rotation keys are s16 units
// shifted left by the file's decimal shift. The material's SRT + centre are
// replaced while the animation is attached (J3DMaterialTable::
// entryTexMtxAnimator also forces the tex-gen matrix to TEXMTX(slot) and the
// Maya flag to the BTK's).
// =============================================================================

#include <revolution/types.h>

#include <cstddef>
#include <string>
#include <vector>

#include "compat/j3d/J3DMathCompat.h"

namespace compat::j3d {

struct BtkEntry {
    std::string materialName;
    u8 texMtxSlot = 0;          // 0..7
    f32 center[3] = {0.5f, 0.5f, 0.5f};
    KeyTrack scale[3];          // S, T, Q
    KeyTrack rotation[3];
    KeyTrack translation[3];
};

class BtkAnim {
public:
    /// Parses a J3D1btk1 file. Returns false with `error` set when malformed.
    bool load(const u8* data, size_t size, std::string* error = nullptr);

    /// Evaluates entry `i` at `frame` (J3DAnmTextureSRTKey::calcTransform).
    void evaluate(size_t i, f32 frame, TexSrt& out) const;

    u8 loopMode = 2;
    u8 rotDecShift = 0;
    u16 duration = 0;
    bool maya = false;
    std::vector<BtkEntry> entries;

private:
    std::vector<f32> mScaleData;
    std::vector<s16> mRotData;
    std::vector<f32> mTransData;
};

} // namespace compat::j3d
