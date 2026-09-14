#pragma once
// =============================================================================
// PC_PORT (M10) — FileSelectHost: the host implementation of the file-select
// screen (Game/Map/FileSelector.cpp + the Screen/ actors it owns).
//
// On the console the FileSelector is a ~1585-line LiveActor with 40 nerves that
// owns FileSelectItem planets, a FileSelectCameraController, the FileInfo bar,
// the FileSelectButton set, a BackButton/BrosButton, the RKNetMii picker and
// the user-file save data. The port cannot run it yet (the ModelManager, J3D
// draw buffers, the message system and RFL are all unported), so this module
// reproduces the screen on the host, driven by the REAL assets and the real
// behaviour constants:
//
//   * layout:   /LayoutData/FileSelect.arc (buttons + the "choose a file" line),
//               /LayoutData/FileInfo.arc (the save bar, layers 1/2 for its
//               Appear/ButtonAppear/ButtonEnd animations),
//               /LayoutData/BackButton.arc, /LayoutData/BrosButton.arc and
//               /LayoutData/FileNumber.arc (the six badge numbers),
//   * models:   compat/j3d/FileSelectField — the planets and the character
//               heads through the host J3D renderer,
//   * camera:   FileSelectCameraController (far / near / title + the 60-frame
//               squared-time moves),
//   * input:    the star pointer, with the console's menu semantics
//               (Platform::CompatInput::getMenuDecideTrigger — A or the
//               pointer's click — and getMenuNav for the D-pad/stick),
//   * text:     compat/game/GameTextTable, so every string on the screen comes
//               from ONE language (the M10.1 bug report: the arc's authored
//               Japanese pane text rendered next to the French start-button
//               string — see GameTextTable.h for the language table and the
//               fix),
//   * audio:    MR::startStageBGM("MBGM_FILE_SELECT") when the items appear and
//               MR::stopStageBGM on the way out (FileSelector::exeTitleEnd /
//               exeFileSelect).
//
// What the screen does, step by step (the animation of the three reference
// captures):
//
//   1. Appear   — the items fade in, the camera flies from the title point to
//                 the far point (the six planets + Mario's head, "Please choose
//                 a file.", no buttons).
//   2. Select   — the pointer picks an item (the pointing cylinder of
//                 FileSelectItem::initStarPointerTarget, radius 1000): the item
//                 scales to 1.2 over 30 frames (ScaleController), the camera
//                 moves to its near point and the FileInfo bar slides in with
//                 that file's name, date and star counts.
//   3. Confirm  — A (or a click) on the item: the items are pushed away
//                 (FileSelector::goToNearPoint -> calcBasePos(-16000)), the
//                 camera follows the selected item and the operation buttons
//                 appear: Play This File / Copy / Icon / Erase, plus Back and
//                 the 2P badge.
//   4. Playing  — "Play This File" on a slot: the host save store is updated
//                 and the screen reports that the game wants to start (the
//                 GameScene is not ported: M10.2 wires it).
// =============================================================================

#include <revolution/types.h>
#include <revolution/mtx.h>

#include <memory>

#include "compat/j3d/FileSelectField.h"

class LayoutManager;
class SimpleLayout;
namespace nw4r {
    namespace lyt {
        class Pane;
        class TextBox;
    }  // namespace lyt
};  // namespace nw4r

namespace compat::game {

/// The screen's state machine (NrvFileSelector subset, one nerve per state).
enum class FileSelectPhase {
    Appear,    // FileSelectorNrvTitleEnd: items + camera flying in
    Select,    // FileSelectorNrvFileSelect: pointing at a file
    Confirm,   // FileSelectorNrvFileConfirm / CreateConfirm: a file is selected
    Playing,   // FileSelectorNrvDemoStartWait: "Play This File" was chosen
    Leaving,   // the screen is done (the scene decides what comes next)
};

/// One slot of the host save store (saves/slotN.bin, format v2 — the v1 files
/// the M10 stand-in wrote are accepted and default to an empty Mario file).
struct FileSelectSaveInfo {
    bool exists = false;
    s32 number = 1;             // 1..6 (the slot on screen)
    s32 character = 0;          // FileSelectCharacter index (0 Mario … 4 Peach)
    s32 powerStars = 0;
    s32 starPieces = 0;
    u64 lastModified = 0;       // seconds since the epoch
    bool clearedNormal = false;
    bool clearedComplete = false;
    wchar_t name[16] = {};
};

class FileSelectHost {
public:
    FileSelectHost();
    ~FileSelectHost();

    /// Mounts the layouts, the 3D field and the save store. Safe to call with
    /// missing assets: every piece degrades (the screen still runs and logs).
    void init();

    /// One 60 Hz game frame. Returns true while the screen should keep running.
    bool update();

    /// Draws the 3D half: the planets and the character heads, through the
    /// file-select camera. Call BEFORE the layout pass of the scene.
    void draw3D();

    /// True while the star pointer is drawn from the game's own DPDPointer
    /// layout (the P1 StarPointer — the white glove holding the blue star).
    /// When false (the arc is missing), the scene draws the immediate-mode
    /// fallback cursor instead.
    bool pointerLayoutActive() const { return mPointerLayoutOk; }

    /// Fallback pointer (immediate-mode glove + star) — only drawn by the
    /// scene when pointerLayoutActive() is false. Call AFTER the layout pass.
    void drawCursor() const;
    /// The StarPointer 1P guidance balloon ("Please choose a file.") under the
    /// planets; also drawn in the 2D pass. Returns false when there is no font
    /// or when the screen is past the select phase.
    bool drawGuidance() const;

    // --- state (diagnostics, the scene and the tests) -----------------------
    FileSelectPhase phase() const { return mPhase; }
    s32 pointedItem() const { return mPointedItem; }
    s32 selectedItem() const { return mSelectedItem; }
    f32 cameraFovy() const;
    bool itemsVisible() const;
    const compat::j3d::FileSelectField& field() const { return *mField; }

    /// The slot currently under the screen's selection (-1: none).
    const FileSelectSaveInfo* pointedSave() const;
    /// Slot chosen with "Play This File" (-1: none yet).
    s32 playingSlot() const { return mPlayingSlot; }

    /// True when the P1 marker (the star pointer the console attaches to the
    /// file a player is working with) has an item to sit on — one is pointed
    /// at, or one is selected.
    bool playerBadgeVisible() const {
        return (mPhase == FileSelectPhase::Confirm ? mSelectedItem : mPointedItem) >= 0;
    }

    /// Save store (host side of GameSequenceFunction::restoreUserFile):
    /// a small binary per slot. Exposed for the tests and for M10.2.
    static bool readSave(s32 slot, FileSelectSaveInfo* out);
    static bool writeSave(const FileSelectSaveInfo& info);
    static bool deleteSave(s32 slot);
    static bool hasSave(s32 slot);

    /// Pointer position in framebuffer pixels (false when there is no pointer).
    bool pointerPixel(f32* outX, f32* outY) const;

private:
    void updatePointer();
    /// Positions the game's StarPointer layout (DPDPointer arc) at the pointer
    /// and shows the target ring over the pointed item — the console's
    /// StarPointerLayout::setPosition + the FileSelector's target circles.
    void updatePointerLayout();
    void updateItems();
    void updateSelectPhase();
    void updateConfirmPhase();
    void enterConfirm();
    void leaveConfirm();
    void enterPlaying();
    void updateCamera();

    /// Pane helpers (pane names come from the arcs; see the .cpp).
    nw4r::lyt::Pane* findPane(const SimpleLayout* pLayout, const char* pPaneName) const;
    /// Invisible panes still draw their children in nw4r (the flag is per pane),
    /// so the operation buttons have to be toggled as a TREE.
    void setPaneTreeVisible(nw4r::lyt::Pane* pPane, bool visible) const;
    void setPanesVisible(const SimpleLayout* pLayout, const char* const* pPaneNames, u32 num, bool visible);
    /// The text box of a badge/info pane, with a fallback to the tree's only
    /// text box when the expected name is not in the arc.
    nw4r::lyt::TextBox* findTextBox(const SimpleLayout* pLayout, const char* pPaneName) const;
    void setText(nw4r::lyt::TextBox* pTextBox, const wchar_t* pText) const;
    void setNumber(nw4r::lyt::TextBox* pTextBox, s32 number) const;
    bool startAnimIfKnown(SimpleLayout* pLayout, const char* pAnimName, u32 layer) const;

    /// Item under the pointer, or -1. Items are hit through the pointing
    /// cylinder the console gives them (FileSelectItem::initStarPointerTarget).
    s32 itemUnderPointer(f32 px, f32 py) const;
    /// Pane rect in framebuffer pixels (false when the pane is not there).
    bool paneRectPixels(const SimpleLayout* pLayout, const char* pPaneName, f32* outX, f32* outY, f32* outW,
                        f32* outH) const;
    bool paneContains(const SimpleLayout* pLayout, const char* pPaneName, f32 px, f32 py) const;

    /// Fills the FileInfo bar for a slot (FileSelector::setFileInfo).
    void applySaveToInfoBar(s32 slot);
    void refreshFieldFromSaves();

    std::unique_ptr< compat::j3d::FileSelectField > mField;

    SimpleLayout* mButtons = nullptr;   // FileSelect.arc: Play/Copy/Icon/Erase + 2P
    SimpleLayout* mInfo = nullptr;      // FileInfo.arc: the save bar
    SimpleLayout* mBack = nullptr;      // BackButton.arc
    SimpleLayout* mBros = nullptr;      // BrosButton.arc (the 2P star)
    SimpleLayout* mBadges[compat::j3d::FileSelectField::kItemNum] = {};  // FileNumber.arc
    SimpleLayout* mPointer = nullptr;   // DPDPointer.arc: the P1 star pointer (the cursor)

    // Layout-space reference points, captured at init.
    struct PaneRef {
        f32 layoutX = 0.0f;   // authored box centre, layout units (Y up)
        f32 layoutY = 0.0f;
        f32 width = 0.0f;
        f32 height = 0.0f;
        bool valid = false;
    };
    PaneRef mBadgeRefs[compat::j3d::FileSelectField::kItemNum];

    // The star-pointer panes, captured at init (DPDPointer arc): the root the
    // cursor is translated to the pointer with, the StarPointer tree (the P1
    // cursor art — nw4r visibility is per pane, so it is toggled, not the
    // root), the HandPointer tree (gameplay only: stays hidden) and the
    // target ring (the cyan circle the console puts around the pointed
    // item's badge).
    nw4r::lyt::Pane* mPointerRoot = nullptr;
    PaneRef mPointerRef;                // authored root translate (compensation)
    nw4r::lyt::Pane* mStarPane = nullptr;
    nw4r::lyt::Pane* mHandPane = nullptr;
    nw4r::lyt::Pane* mRingPane = nullptr;
    PaneRef mRingRef;                   // the ring's ancestors' authored offset
    bool mPointerLayoutOk = false;      // the arc mounted and has a StarPointer pane

    FileSelectPhase mPhase = FileSelectPhase::Appear;
    s32 mPointedItem = -1;
    s32 mSelectedItem = -1;
    s32 mPlayingSlot = -1;

    // Camera (FileSelectCameraController): the current state plus the move.
    compat::j3d::FileSelectCamera mCamera;
    compat::j3d::FileSelectCamera mMoveFrom;
    compat::j3d::FileSelectCamera mMoveTo;
    s32 mMoveStep = -1;         // -1: not moving
    f32 mAppearTimer = 0.0f;

    // Pointer (framebuffer pixels, top-left origin) + edges.
    f32 mPointerX = 0.0f;
    f32 mPointerY = 0.0f;
    bool mHavePointer = false;
    bool mLastDecide = false;
    bool mLastCancel = false;
    s32 mInfoSlot = -1;         // slot the info bar currently shows
    bool mInfoVisible = false;
    bool mButtonsVisible = false;

    u32 mFrame = 0;
    bool mInitialized = false;
    bool mBgmStarted = false;
};

}  // namespace compat::game
