// =============================================================================
// PC_PORT (M10) — FileSelectHost implementation (see FileSelectHost.h).
// =============================================================================

#include "compat/game/FileSelectHost.h"

#include "compat/game/UiAnchoring.h"
#include "compat/kpad/KPADCompat.h"
#include "platform/Log/Log.h"
#include "platform/Timing/Timing.h"

#include <revolution/gx.h>
#include <revolution/mtx.h>
#include <revolution/wpad.h>

#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

namespace compat::game {

namespace {

    constexpr int kSlotCount = 3;

    std::string savePath(int slot) {
        return "saves/slot" + std::to_string(slot) + ".bin";
    }

    void writeSaveStub(int slot) {
        // Host save store v1: a marker file per slot. The real SMG save
        // (MR::SaveData, galaxy progress, Mii) lands with the GameScene work;
        // the FileSelect screen only needs to know WHICH slots exist.
        std::error_code ec;
        std::filesystem::create_directories("saves", ec);
        std::ofstream out(savePath(slot), std::ios::binary | std::ios::trunc);
        if (!out) {
            PL_LOG_WARN("fileselect", "could not write %s", savePath(slot).c_str());
            return;
        }
        const char magic[8] = {'L', 'U', 'M', 'A', 'S', 'A', 'V', 'E'};
        out.write(magic, sizeof(magic));
        const u32 version = 1;
        const u32 slotU32 = static_cast<u32>(slot);
        const u64 stamp = static_cast<u64>(std::time(nullptr));
        out.write(reinterpret_cast<const char*>(&version), sizeof(version));
        out.write(reinterpret_cast<const char*>(&slotU32), sizeof(slotU32));
        out.write(reinterpret_cast<const char*>(&stamp), sizeof(stamp));
        PL_LOG_INFO("fileselect", "host save written: %s", savePath(slot).c_str());
    }

}  // namespace

bool FileSelectHost::hasSave(int slot) const {
    if (slot < 0 || slot >= kSlotCount) {
        return false;
    }
    std::ifstream in(savePath(slot), std::ios::binary);
    return in.good();
}

bool FileSelectHost::update() {
    // Pointer (mouse / stick through the KPAD DPD mapping).
    float px = 0.0f;
    float py = 0.0f;
    if (Platform::CompatInput::getPointerPos(0, &px, &py)) {
        mPointerX = px;
        mPointerY = py;
        mHavePointer = true;
    }

    // Provisional slot mapping: screen thirds on the pointer's x. Replaced by
    // pane-accurate hit rects once the FileSelect.arc pane tree is known from
    // a runtime dump (the layout manager logs it on build).
    const float x = mPointerX;
    mSlot = x < -0.25f ? 0 : (x > 0.25f ? 2 : 1);

    const u32 trig = Platform::CompatInput::getTrigButtons(0);
    if ((trig & WPAD_BUTTON_A) != 0) {
        mConfirmedSlot = mSlot;
        PL_LOG_INFO("fileselect", "A confirmed on slot %d (pointer %.2f, %.2f)", mSlot,
                    mPointerX, mPointerY);
        writeSaveStub(mSlot);
    }
    if ((trig & WPAD_BUTTON_B) != 0) {
        mBackRequested = true;
        PL_LOG_INFO("fileselect", "B pressed — console replays the title here (v1: log only)");
    }

    return true;
}

void FileSelectHost::drawCursor() const {
    if (!mHavePointer) {
        return;  // no input source (headless): nothing to point with
    }

    // Pixel ortho over the framebuffer, same space as the title backdrop: the
    // pointer's [-1,1] maps to the whole presented window.
    f32 width = 0.0f;
    f32 height = 0.0f;
    compat::ui::framebufferSize(&width, &height);
    if (width <= 0.0f || height <= 0.0f) {
        return;
    }

    const f32 cx = (mPointerX * 0.5f + 0.5f) * width;
    const f32 cy = (1.0f - (mPointerY * 0.5f + 0.5f)) * height;

    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);

    Mtx mtxImm;
    PSMTXIdentity(mtxImm);
    GXLoadPosMtxImm(mtxImm, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);

    Mtx44 projMtx;
    C_MTXOrtho(projMtx, 0.0f, height, 0.0f, width, -1.0f, 1.0f);
    GXSetProjection(projMtx, GX_ORTHOGRAPHIC);

    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_VTX, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE,
                  GX_AF_NONE);
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_SET);
    GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GXSetCullMode(GX_CULL_NONE);

    // Star-cursor stand-in: a warm disc with a white core, breathing slightly
    // so it reads as alive. The real StarPointer actor (its own arc) lands
    // with the pane-accurate iteration.
    const f32 t = static_cast<f32>(Platform::Timing::nowSeconds());
    const f32 radius = height * 0.020f * (1.0f + 0.06f * sinf(t * 6.0f));
    constexpr int kSeg = 20;

    GXBegin(GX_TRIANGLEFAN, GX_VTXFMT0, kSeg + 2);
    GXPosition2f32(cx, cy);
    GXColor4u8(255, 255, 240, 235);
    for (int i = 0; i <= kSeg; ++i) {
        const f32 a = static_cast<f32>(i) * (2.0f * 3.14159265f / static_cast<f32>(kSeg));
        GXPosition2f32(cx + sinf(a) * radius, cy + cosf(a) * radius);
        GXColor4u8(255, 214, 90, 190);
    }
    GXEnd();
}

}  // namespace compat::game
