// =============================================================================
// PC_PORT (M10) — FileSelectHost implementation (see FileSelectHost.h).
// =============================================================================

#include "compat/game/FileSelectHost.h"

#include "Game/Screen/LayoutManager.hpp"
#include "Game/Util/LayoutUtil.hpp"
#include "Game/Screen/SimpleLayout.hpp"
#include "Game/Util/SoundUtil.hpp"

#include "compat/game/GameTextTable.h"
#include "compat/game/UiAnchoring.h"
#include "compat/kpad/KPADCompat.h"
#include "compat/ui/GuidanceBanner.h"
#include "compat/j3d/BmdRenderer.h"
#include "platform/Log/Log.h"
#include "platform/Timing/Timing.h"

#include <nw4r/lyt/layout.h>
#include <nw4r/lyt/pane.h>
#include <nw4r/lyt/textBox.h>

#include <revolution/gx.h>
#include <revolution/mtx.h>
#include <revolution/wpad.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

namespace compat::game {

namespace {

constexpr s32 kSlotCount = compat::j3d::FileSelectField::kItemNum;

// The panes FileSelect.arc shows only once a file is selected: the four
// operation buttons (FileSelectButton: StartButton + Copy/Mii/Delete) and the
// 2P badge of the layout. Reference capture 1 (nothing pointed) has none of
// them, capture 3 (file selected) has all of them — this is the toggle.
// NOTE they are toggled as TREES: nw4r draws a pane's children even when the
// parent's own visibility flag is off.
const char* const kOperationPanes[] = {"StartButton", "CopyButton", "MiiButton", "DeleteButton", "2P"};
constexpr u32 kOperationPaneNum = sizeof(kOperationPanes) / sizeof(kOperationPanes[0]);

// --- the save store ---------------------------------------------------------
// M10 wrote a marker file ("LUMASAVE", v1). v2 adds what the FileInfo bar and
// the item models need: the character (which head the slot shows), the star
// counts and the timestamp (the date/time the bar prints).
constexpr char kSaveMagic[8] = {'L', 'U', 'M', 'A', 'S', 'A', 'V', 'E'};
constexpr u32 kSaveVersion = 2;

std::string savePath(s32 slot) {
    return "saves/slot" + std::to_string(slot) + ".bin";
}

template < typename T >
void readPod(std::istream& in, T& out) {
    in.read(reinterpret_cast< char* >(&out), sizeof(T));
}

template < typename T >
void writePod(std::ostream& out, const T& value) {
    out.write(reinterpret_cast< const char* >(&value), sizeof(T));
}

// --- pane helpers -----------------------------------------------------------

/// Walks the parent chain and sums the local translations: the pane's authored
/// position in layout units, without depending on a calcAnim pass having run.
void paneAuthoredTranslate(const nw4r::lyt::Pane* pPane, f32* outX, f32* outY) {
    f32 x = 0.0f;
    f32 y = 0.0f;

    for (const nw4r::lyt::Pane* p = pPane; p != nullptr; p = p->GetParent()) {
        x += p->mTranslate.x;
        y += p->mTranslate.y;
    }

    if (outX != nullptr) {
        *outX = x;
    }
    if (outY != nullptr) {
        *outY = y;
    }
}

/// Sizes in layout units (the badge/info panes are authored in the design
/// space, so no conversion is needed).
nw4r::lyt::Pane* findPaneRecursive(nw4r::lyt::Pane* pPane, const char* pName) {
    if (pPane == nullptr) {
        return nullptr;
    }

    if (pPane->mName[0] != '\0' && std::strcmp(pPane->mName, pName) == 0) {
        return pPane;
    }

    for (auto it = pPane->mChildList.GetBeginIter(); it != pPane->mChildList.GetEndIter(); ++it) {
        if (nw4r::lyt::Pane* pFound = findPaneRecursive(&*it, pName)) {
            return pFound;
        }
    }

    return nullptr;
}

/// First text box under a pane (depth-first): the layout family puts the text
/// of a badge/button in exactly one text box, so this is what to write to when
/// the arc names it differently than MR::setTextBoxMessageRecursive expects.
nw4r::lyt::TextBox* firstTextBoxUnder(nw4r::lyt::Pane* pPane) {
    if (pPane == nullptr) {
        return nullptr;
    }

    for (auto it = pPane->mChildList.GetBeginIter(); it != pPane->mChildList.GetEndIter(); ++it) {
        nw4r::lyt::Pane* pChild = &*it;

        if (pChild->GetRuntimeTypeInfo()->IsDerivedFrom(&nw4r::lyt::TextBox::typeInfo)) {
            return static_cast< nw4r::lyt::TextBox* >(pChild);
        }

        if (nw4r::lyt::TextBox* pFound = firstTextBoxUnder(pChild)) {
            return pFound;
        }
    }

    return nullptr;
}

// --- the pointer cursor (fallback) -------------------------------------------
//
// The pointer is the game's own StarPointer: the "StarPointer" pane of the
// DPDPointer layout arc (the white glove holding the blue star — the P1 cursor
// of the reference captures), mounted and positioned by updatePointerLayout.
// The immediate-mode glove below is only drawn when that arc cannot be mounted
// (it keeps the screen usable on a partial dump).
void drawQuad(f32 x, f32 y, f32 w, f32 h, u8 r, u8 g, u8 b, u8 a) {
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    GXPosition2f32(x, y);
    GXColor4u8(r, g, b, a);
    GXPosition2f32(x + w, y);
    GXColor4u8(r, g, b, a);
    GXPosition2f32(x + w, y + h);
    GXColor4u8(r, g, b, a);
    GXPosition2f32(x, y + h);
    GXColor4u8(r, g, b, a);
    GXEnd();
}

void drawStar(f32 cx, f32 cy, f32 outer, f32 inner, u8 r, u8 g, u8 b, u8 a) {
    constexpr int kPoints = 5;
    GXBegin(GX_TRIANGLEFAN, GX_VTXFMT0, static_cast< u16 >(kPoints * 2 + 2));
    GXPosition2f32(cx, cy);
    GXColor4u8(r, g, b, a);

    for (int i = 0; i <= kPoints * 2; ++i) {
        const f32 ang = static_cast< f32 >(i) * (3.14159265f / static_cast< f32 >(kPoints)) - 1.5707963f;
        const f32 radius = (i % 2 == 0) ? outer : inner;
        GXPosition2f32(cx + std::cos(ang) * radius, cy - std::sin(ang) * radius);
        GXColor4u8(r, g, b, a);
    }
    GXEnd();
}

void drawDisc(f32 cx, f32 cy, f32 radius, u8 r, u8 g, u8 b, u8 a) {
    constexpr int kSeg = 24;
    GXBegin(GX_TRIANGLEFAN, GX_VTXFMT0, kSeg + 2);
    GXPosition2f32(cx, cy);
    GXColor4u8(r, g, b, a);

    for (int i = 0; i <= kSeg; ++i) {
        const f32 ang = static_cast< f32 >(i) * (2.0f * 3.14159265f / static_cast< f32 >(kSeg));
        GXPosition2f32(cx + std::sin(ang) * radius, cy + std::cos(ang) * radius);
        GXColor4u8(r, g, b, a);
    }
    GXEnd();
}

}  // namespace

// =============================================================================
// Save store
// =============================================================================

bool FileSelectHost::readSave(s32 slot, FileSelectSaveInfo* out) {
    if (out == nullptr || slot < 1 || slot > kSlotCount) {
        return false;
    }

    *out = FileSelectSaveInfo();
    out->number = slot;

    std::ifstream in(savePath(slot), std::ios::binary);

    if (!in) {
        return false;
    }

    char magic[8] = {};
    u32 version = 0;
    u32 slotU32 = 0;
    readPod(in, magic);
    readPod(in, version);
    readPod(in, slotU32);

    if (std::memcmp(magic, kSaveMagic, sizeof(magic)) != 0) {
        PL_LOG_WARN("fileselect", "save slot %d is not a LUMA save — ignored", slot);
        return false;
    }

    out->exists = true;
    out->number = static_cast< s32 >(slotU32);

    if (version == 1) {
        // The M10 stand-in: a marker plus a timestamp, no game data. Show it as
        // an empty Mario file so the screen stays readable.
        u64 stamp = 0;
        readPod(in, stamp);
        out->lastModified = stamp;
        out->character = 0;
        std::swprintf(out->name, sizeof(out->name) / sizeof(wchar_t), L"Mario");
        return true;
    }

    u32 character = 0;
    s32 powerStars = 0;
    s32 starPieces = 0;
    u64 stamp = 0;
    u8 flags = 0;
    u16 name[16] = {};

    readPod(in, character);
    readPod(in, powerStars);
    readPod(in, starPieces);
    readPod(in, stamp);
    readPod(in, flags);
    in.read(reinterpret_cast< char* >(name), sizeof(name));

    out->character = static_cast< s32 >(character);
    out->powerStars = powerStars;
    out->starPieces = starPieces;
    out->lastModified = stamp;
    out->clearedNormal = (flags & 1) != 0;
    out->clearedComplete = (flags & 2) != 0;

    for (int i = 0; i < 16; ++i) {
        out->name[i] = static_cast< wchar_t >(name[i]);
    }

    out->name[15] = L'\0';

    return true;
}

bool FileSelectHost::writeSave(const FileSelectSaveInfo& info) {
    if (info.number < 1 || info.number > kSlotCount) {
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories("saves", ec);

    std::ofstream out(savePath(info.number), std::ios::binary | std::ios::trunc);

    if (!out) {
        PL_LOG_WARN("fileselect", "could not write %s", savePath(info.number).c_str());
        return false;
    }

    const u32 slotU32 = static_cast< u32 >(info.number);
    const u32 character = static_cast< u32 >(info.character);
    const u8 flags = static_cast< u8 >((info.clearedNormal ? 1 : 0) | (info.clearedComplete ? 2 : 0));
    u16 name[16] = {};

    for (int i = 0; i < 16; ++i) {
        name[i] = static_cast< u16 >(info.name[i]);
    }

    out.write(kSaveMagic, sizeof(kSaveMagic));
    writePod(out, kSaveVersion);
    writePod(out, slotU32);
    writePod(out, character);
    writePod(out, info.powerStars);
    writePod(out, info.starPieces);
    writePod(out, info.lastModified);
    writePod(out, flags);
    out.write(reinterpret_cast< const char* >(name), sizeof(name));

    PL_LOG_INFO("fileselect", "save slot %d written (%d stars, %d star pieces, character %u)", info.number,
                info.powerStars, info.starPieces, static_cast< unsigned >(character));

    return true;
}

bool FileSelectHost::deleteSave(s32 slot) {
    std::error_code ec;
    const bool removed = std::filesystem::remove(savePath(slot), ec);

    PL_LOG_INFO("fileselect", "erase slot %d: %s", slot, removed ? "file removed" : "no file");

    return removed;
}

bool FileSelectHost::hasSave(s32 slot) {
    FileSelectSaveInfo info;

    return readSave(slot, &info);
}

// =============================================================================
// Screen
// =============================================================================

FileSelectHost::FileSelectHost() = default;
FileSelectHost::~FileSelectHost() = default;

void FileSelectHost::init() {
    if (mInitialized) {
        return;
    }

    mInitialized = true;

    // --- the 3D field (planets + character heads) ---------------------------
    mField.reset(new compat::j3d::FileSelectField());

    if (!mField->init()) {
        PL_LOG_WARN("fileselect",
                    "the planet archive is missing — the screen falls back to the layout-only look "
                    "(FileSelect.arc was mounted, /ObjectData/FileSelectDataPlanet.arc was not)");
    }

    refreshFieldFromSaves();

    // --- the layouts --------------------------------------------------------
    // Same mount path as the M10 stand-in (SimpleLayout -> LayoutManager), now
    // with every arc the console screen uses. A missing arc degrades to the
    // null-layout path (LayoutManager tolerates a null mLayout).
    mButtons = new SimpleLayout("FileSelectButtons", "FileSelect", 1, -1);
    mButtons->appear();

    mInfo = new SimpleLayout("FileSelectInfo", "FileInfo", 3, -1);
    mInfo->appear();

    mBack = new SimpleLayout("FileSelectBack", "BackButton", 1, -1);
    mBack->appear();

    mBros = new SimpleLayout("FileSelectBros", "BrosButton", 1, -1);
    mBros->appear();

    // The star badge reads "P2" in every language (the reference capture shows
    // it unchanged next to "Play This File"); whatever the brlyt baked in (or
    // the language of the pane that survived removeUnnecessaryPanes), force the
    // one wording the screen shows on the console.
    if (nw4r::lyt::Pane* pBrosRoot = findPane(mBros, "RootPane")) {
        if (nw4r::lyt::TextBox* pBrosText = firstTextBoxUnder(pBrosRoot)) {
            setText(pBrosText, L"P2");
        }
    }

    for (s32 i = 0; i < kSlotCount; ++i) {
        mBadges[i] = new SimpleLayout("FileSelectNumber", "FileNumber", 2, -1);
        mBadges[i]->appear();

        // The badge's reference point: its box centre in layout units, taken
        // from the authored transform (no calcAnim needed).
        nw4r::lyt::Pane* pPane = findPane(mBadges[i], "FileNumber");

        if (pPane == nullptr) {
            pPane = findPane(mBadges[i], "RootPane");
        }

        if (pPane != nullptr) {
            f32 ax = 0.0f;
            f32 ay = 0.0f;
            paneAuthoredTranslate(pPane, &ax, &ay);
            mBadgeRefs[i].layoutX = ax;
            mBadgeRefs[i].layoutY = ay - pPane->mSize.height * 0.5f;  // box centre
            mBadgeRefs[i].width = pPane->mSize.width;
            mBadgeRefs[i].height = pPane->mSize.height;
            mBadgeRefs[i].valid = true;
        }

        if (nw4r::lyt::TextBox* pNumber = findTextBox(mBadges[i], "FileNumber")) {
            setNumber(pNumber, i + 1);
        }
    }

    // --- the star pointer (the P1 cursor) — the game's own art ---------------
    // On the console the FileSelector's pointer is the StarPointer actor:
    // changeToStarPointer() shows the "StarPointer" pane of the DPDPointer
    // layout arc (the white glove holding the blue star) and
    // setPosition() translates it to the Wiimote pointer every frame. The
    // port mounts that same arc and the same panes, so the cursor is game art
    // only. It is created LAST among the screen's layouts, which makes the
    // layout pass draw it over everything (the console's pointer is on top
    // too). When the arc is unavailable the screen falls back to the
    // immediate-mode glove in drawCursor().
    mPointer = new SimpleLayout("FileSelectStarPointer", "DPDPointer", 1, -1);
    mPointer->appear();

    LayoutManager* pPointerManager = mPointer->getLayoutManager();

    if (pPointerManager != nullptr && pPointerManager->mLayout != nullptr &&
        pPointerManager->mLayout->mpRootPane != nullptr) {
        mPointerRoot = pPointerManager->mLayout->mpRootPane;

        f32 ax = 0.0f;
        f32 ay = 0.0f;
        paneAuthoredTranslate(mPointerRoot, &ax, &ay);
        mPointerRef.layoutX = ax;
        mPointerRef.layoutY = ay;
        mPointerRef.valid = true;

        // StarPointerLayout::changeToStarPointer: the StarPointer tree is the
        // cursor, the HandPointer tree (the gameplay hand, with the player
        // number pictures) is hidden. nw4r's visibility is per pane (a child
        // draws even when its parent is off), so both trees are toggled as
        // trees — the StarPointer one starts off until the pointer is live
        // (updatePointerLayout turns it on).
        bool starPane = false;

        if (nw4r::lyt::Pane* pStar = findPaneRecursive(mPointerRoot, "StarPointer")) {
            mStarPane = pStar;
            setPaneTreeVisible(pStar, false);
            starPane = true;
        }

        if (nw4r::lyt::Pane* pHand = findPaneRecursive(mPointerRoot, "HandPointer")) {
            mHandPane = pHand;
            setPaneTreeVisible(pHand, false);
        }

        // The target ring: the cyan circle the console puts around the pointed
        // item's badge (StarPointerUtil::addStarPointerTargetCircle drives the
        // layout's GroupRing group). Positioned per frame in
        // updatePointerLayout; hidden until something is pointed.
        mRingPane = findPaneRecursive(mPointerRoot, "GroupRing");

        if (mRingPane == nullptr) {
            mRingPane = findPaneRecursive(mPointerRoot, "Ring");
        }

        if (mRingPane != nullptr) {
            // The compensation is the ring's ANCESTORS' offset (its own local
            // translate is what we write every frame, relative to the parent).
            f32 fullX = 0.0f;
            f32 fullY = 0.0f;
            paneAuthoredTranslate(mRingPane, &fullX, &fullY);
            mRingRef.layoutX = fullX - mRingPane->mTranslate.x;
            mRingRef.layoutY = fullY - mRingPane->mTranslate.y;
            mRingRef.valid = true;
            setPaneTreeVisible(mRingPane, false);
        }

        mPointerLayoutOk = starPane;
        mPointerRoot->SetVisible(false);  // appears once the pointer is live

        if (!starPane) {
            PL_LOG_WARN("fileselect", "DPDPointer layout has no 'StarPointer' pane — using the drawn fallback cursor");
        }
    } else {
        PL_LOG_WARN("fileselect", "DPDPointer layout did not mount — using the drawn fallback cursor");
    }

    // --- the operation buttons start hidden ---------------------------------
    // On the console they belong to FileSelectButton, which appears only once a
    // file is selected (FileSelector::onSelect -> FileConfirmStart). The M10
    // stand-in drew them from the first frame, which is what the reference
    // capture 1 (no buttons) contradicts.
    setPanesVisible(mButtons, kOperationPanes, kOperationPaneNum, false);
    mButtonsVisible = false;

    // The 2P badge and the back button wait for the confirm screen too.
    if (nw4r::lyt::Pane* pRoot = findPane(mBack, "RootPane")) {
        pRoot->SetVisible(false);
    }
    if (nw4r::lyt::Pane* pRoot = findPane(mBros, "RootPane")) {
        pRoot->SetVisible(false);
    }

    // --- camera + pointer ---------------------------------------------------
    mCamera = compat::j3d::FileSelectField::cameraTitle();
    mMoveFrom = mCamera;
    mMoveTo = compat::j3d::FileSelectField::cameraFar();
    mMoveStep = 0;

    PL_LOG_INFO("fileselect",
                "file-select screen mounted: FileSelect/FileInfo/BackButton/BrosButton/FileNumber arcs + "
                "3D field (%s) + star pointer (%s)",
                mField->loaded() ? "models loaded" : "no planet models",
                mPointerLayoutOk ? "DPDPointer layout (game art)" : "drawn fallback");

    // The console starts the FileSelect BGM when the items appear
    // (FileSelector::exeTitleEnd). Without the MBGM label mapping this call used
    // to resolve to a file that does not exist — the M10.1 "no music" report.
    MR::startStageBGM("MBGM_FILE_SELECT", false);
    mBgmStarted = true;
}

bool FileSelectHost::update() {
    if (!mInitialized) {
        init();
    }

    ++mFrame;
    mAppearTimer += 1.0f / 60.0f;

    updatePointer();
    updatePointerLayout();
    updateCamera();
    updateItems();

    switch (mPhase) {
    case FileSelectPhase::Appear:
        // FileSelector::exeTitleEnd -> the items appear, then the screen waits
        // for the pointer (FileSelect).
        mField->setAppearRate(mAppearTimer / 0.75f);

        if (mAppearTimer >= 0.75f) {
            mPhase = FileSelectPhase::Select;
            PL_LOG_INFO("fileselect", "items in place — FileSelect phase (point at a file)");
        }
        break;

    case FileSelectPhase::Select:
        updateSelectPhase();
        break;

    case FileSelectPhase::Confirm:
        updateConfirmPhase();
        break;

    case FileSelectPhase::Playing:
    case FileSelectPhase::Leaving:
        break;
    }

    // The save store is the source of truth for the item heads (a file created
    // or erased on this screen updates what the planets show).
    if (mFrame % 30 == 0) {
        refreshFieldFromSaves();
    }

    return mPhase != FileSelectPhase::Leaving;
}

void FileSelectHost::refreshFieldFromSaves() {
    if (!mField) {
        return;
    }

    for (s32 i = 0; i < kSlotCount; ++i) {
        FileSelectSaveInfo info;
        const bool exists = readSave(i + 1, &info);

        mField->setHasSave(i, exists);

        if (exists) {
            const s32 ch = info.character;
            mField->setCharacter(i, static_cast< compat::j3d::FileSelectCharacter >(
                                       ch >= 0 && ch < static_cast< s32 >(compat::j3d::FileSelectCharacter::Count)
                                           ? ch
                                           : 0));
        }
    }
}

void FileSelectHost::updatePointer() {
    f32 x = 0.0f;
    f32 y = 0.0f;

    if (Platform::CompatInput::getPointerPos(0, &x, &y)) {
        f32 fbWidth = 0.0f;
        f32 fbHeight = 0.0f;
        compat::ui::framebufferSize(&fbWidth, &fbHeight);

        mPointerX = (x * 0.5f + 0.5f) * fbWidth;
        mPointerY = (1.0f - (y * 0.5f + 0.5f)) * fbHeight;
        mHavePointer = true;
    }

    mLastDecide = Platform::CompatInput::getMenuDecideTrigger(0);
    mLastCancel = Platform::CompatInput::getMenuCancelTrigger(0);

    // Menu navigation (D-pad / stick): moves the selection without the pointer,
    // which is how a gamepad-only player picks a file.
    const int nav = Platform::CompatInput::getMenuNav(0);

    if (nav != 0 && mPhase == FileSelectPhase::Select) {
        const int forwardBits = Platform::CompatInput::kMenuNavRight | Platform::CompatInput::kMenuNavDown;
        const s32 step = (nav & forwardBits) != 0 ? 1 : -1;
        const s32 previous = mPointedItem;
        mPointedItem = (mPointedItem + step + kSlotCount) % kSlotCount;

        // Same consequences as pointing at the item with the pointer: the info
        // bar loads that file's data (handled on the next updateSelectPhase by
        // the pointer path, so the bar stays in sync either way).
        PL_LOG_INFO("fileselect", "menu nav: selection moved from %d to item %d", previous + 1, mPointedItem + 1);
    }
}

void FileSelectHost::updatePointerLayout() {
    if (!mPointerLayoutOk || mPointerRoot == nullptr) {
        return;
    }

    f32 fbWidth = 0.0f;
    f32 fbHeight = 0.0f;
    compat::ui::framebufferSize(&fbWidth, &fbHeight);

    if (fbWidth <= 0.0f || fbHeight <= 0.0f) {
        return;
    }

    // The cursor follows the pointer (StarPointerLayout::setPosition ->
    // setTrans: the root pane is translated to the pointer's layout position).
    // The StarPointer tree itself is what makes the cursor visible (nw4r: a
    // child draws even when its parent is off, so the tree is toggled).
    mPointerRoot->SetVisible(mHavePointer);

    if (mStarPane != nullptr) {
        setPaneTreeVisible(mStarPane, mHavePointer);
    }

    if (mHandPane != nullptr) {
        setPaneTreeVisible(mHandPane, false);
    }

    if (!mHavePointer) {
        if (mRingPane != nullptr) {
            setPaneTreeVisible(mRingPane, false);
        }

        return;
    }

    const f32 scale = compat::ui::uiScale(fbWidth, fbHeight);
    const f32 wantX = (mPointerX - fbWidth * 0.5f) / scale;
    const f32 wantY = (fbHeight * 0.5f - mPointerY) / scale;

    // Exactly what the console does (LayoutActor::setTrans): the root pane's
    // translate is REPLACED by the pointer's layout position — the brlyt is
    // authored with the hot spot at the root's local origin, so no
    // compensation.
    mPointerRoot->mTranslate.x = wantX;
    mPointerRoot->mTranslate.y = wantY;
    mPointerRoot->mTranslate.z = 0.0f;

    // The target ring over the pointed item (reference capture 2: the cyan
    // circle around the pointed badge). Hidden while nothing is pointed and
    // in the confirm state (capture 3 has no ring — the items are pushed away).
    if (mRingPane == nullptr) {
        return;
    }

    const bool ringUp = (mPhase == FileSelectPhase::Select) && (mPointedItem >= 0) && (mField != nullptr);

    if (!ringUp) {
        setPaneTreeVisible(mRingPane, false);
        return;
    }

    f32 bx = 0.0f;
    f32 by = 0.0f;
    f32 bz = 0.0f;
    compat::j3d::FileSelectField::calcBadgeWorldPos(mPointedItem, mField->zShift(), &bx, &by, &bz);

    const compat::j3d::FileSelectItemScreen screen =
        compat::j3d::FileSelectField::project(mCamera, bx, by, bz, fbWidth, fbHeight);

    if (!screen.visible) {
        setPaneTreeVisible(mRingPane, false);
        return;
    }

    const f32 ringX = (screen.x - fbWidth * 0.5f) / scale;
    const f32 ringY = (fbHeight * 0.5f - screen.y) / scale;

    mRingPane->mTranslate.x = ringX - mRingRef.layoutX;
    mRingPane->mTranslate.y = ringY - mRingRef.layoutY;
    mRingPane->mTranslate.z = 0.0f;
    setPaneTreeVisible(mRingPane, true);
}

void FileSelectHost::updateItems() {
    if (!mField) {
        return;
    }

    mField->update();

    // The badges follow their items on screen (FileSelectNumber is a layout the
    // console places through the item's 3D position).
    f32 fbWidth = 0.0f;
    f32 fbHeight = 0.0f;
    compat::ui::framebufferSize(&fbWidth, &fbHeight);

    if (fbWidth <= 0.0f || fbHeight <= 0.0f) {
        return;
    }

    const f32 scale = compat::ui::uiScale(fbWidth, fbHeight);

    for (s32 i = 0; i < kSlotCount; ++i) {
        if (mBadges[i] == nullptr || !mBadgeRefs[i].valid) {
            continue;
        }

        f32 bx = 0.0f;
        f32 by = 0.0f;
        f32 bz = 0.0f;
        compat::j3d::FileSelectField::calcBadgeWorldPos(i, mField->zShift(), &bx, &by, &bz);

        const compat::j3d::FileSelectItemScreen screen =
            compat::j3d::FileSelectField::project(mCamera, bx, by, bz, fbWidth, fbHeight);

        nw4r::lyt::Pane* pRoot = findPane(mBadges[i], "RootPane");

        if (pRoot == nullptr) {
            continue;
        }

        if (!screen.visible || mPhase == FileSelectPhase::Confirm) {
            pRoot->SetVisible(false);
            continue;
        }

        pRoot->SetVisible(true);

        // Layout units the badge box centre has to land on (Y up, centred).
        const f32 wantX = (screen.x - fbWidth * 0.5f) / scale;
        const f32 wantY = (fbHeight * 0.5f - screen.y) / scale;

        pRoot->mTranslate.x = wantX - mBadgeRefs[i].layoutX;
        pRoot->mTranslate.y = wantY - mBadgeRefs[i].layoutY;
        pRoot->mTranslate.z = 0.0f;
    }
}

void FileSelectHost::updateCamera() {
    if (mMoveStep < 0) {
        return;
    }

    // The move runs over the 45 frames the items take to appear (0.75 s), with
    // the console's squared-time easing: the far point (the reference view) is
    // exactly in place when the Appear phase ends and the screen becomes
    // selectable.
    const f32 t = static_cast< f32 >(mMoveStep) / 45.0f;
    mCamera = compat::j3d::FileSelectField::blendCamera(mMoveFrom, mMoveTo, t);
    ++mMoveStep;

    if (mMoveStep > 45) {
        mCamera = mMoveTo;
        mMoveStep = -1;
    }
}

f32 FileSelectHost::cameraFovy() const {
    return mCamera.fovy;
}

bool FileSelectHost::itemsVisible() const {
    return mField != nullptr && mField->visible();
}

const FileSelectSaveInfo* FileSelectHost::pointedSave() const {
    static FileSelectSaveInfo sInfo;

    if (mPointedItem < 0) {
        return nullptr;
    }

    readSave(mPointedItem + 1, &sInfo);

    return &sInfo;
}

// -----------------------------------------------------------------------------
// Select: point at a planet, the info bar slides in, the camera flies closer.
// -----------------------------------------------------------------------------
void FileSelectHost::updateSelectPhase() {
    const s32 previous = mPointedItem;

    if (mHavePointer) {
        mPointedItem = itemUnderPointer(mPointerX, mPointerY);
    }

    if (mPointedItem != previous) {
        if (previous >= 0 && mField != nullptr) {
            mField->setScale(previous, 1.0f);  // ScaleController exeToSmall
        }

        if (mPointedItem >= 0) {
            MR::startSystemSE("SE_SY_BUTTON_CURSOR_ON");

            if (mField != nullptr) {
                mField->setScale(mPointedItem, 1.2f);  // exeToBig
            }

            applySaveToInfoBar(mPointedItem + 1);

            // Camera: FileSelector's FileSelect nerve flies to the near point of
            // the pointed item (FileSelectCameraController::goToNearPoint).
            f32 ix = 0.0f;
            f32 iy = 0.0f;
            f32 iz = 0.0f;
            compat::j3d::FileSelectField::calcItemWorldPos(mPointedItem, mField != nullptr ? mField->zShift() : 0.0f,
                                                           &ix, &iy, &iz);
            mMoveFrom = mCamera;
            mMoveTo = compat::j3d::FileSelectField::cameraNear(ix, iy, iz);
            mMoveStep = 0;
        } else {
            // Nothing pointed: the bar slides out and the camera returns to the
            // far point (FileSelector::clearPointing + the FileSelect nerve).
            if (mInfoVisible) {
                startAnimIfKnown(mInfo, "ButtonEnd", 1);
                mInfoVisible = false;
                mInfoSlot = -1;
            }

            mMoveFrom = mCamera;
            mMoveTo = compat::j3d::FileSelectField::cameraFar();
            mMoveStep = 0;
        }
    }

    if (mLastDecide && mPointedItem >= 0) {
        mSelectedItem = mPointedItem;
        enterConfirm();
    }
}

void FileSelectHost::enterConfirm() {
    mPhase = FileSelectPhase::Confirm;

    // FileSelector::onSelect -> FileConfirmStart + goToNearPoint:
    // calcBasePos(-16000) pushes the fan away, the camera follows the selected
    // item, and the operation buttons appear.
    if (mField != nullptr) {
        mField->setZShift(-16000.0f);
        mField->setScale(mSelectedItem, 1.0f);
    }

    f32 ix = 0.0f;
    f32 iy = 0.0f;
    f32 iz = 0.0f;
    compat::j3d::FileSelectField::calcItemWorldPos(mSelectedItem, -16000.0f, &ix, &iy, &iz);

    mMoveFrom = mCamera;
    mMoveTo = compat::j3d::FileSelectField::cameraNear(ix, iy, iz);
    mMoveStep = 0;

    setPanesVisible(mButtons, kOperationPanes, kOperationPaneNum, true);
    mButtonsVisible = true;

    if (nw4r::lyt::Pane* pRoot = findPane(mBack, "RootPane")) {
        pRoot->SetVisible(true);
    }
    if (nw4r::lyt::Pane* pRoot = findPane(mBros, "RootPane")) {
        pRoot->SetVisible(true);
    }

    startAnimIfKnown(mButtons, "Appear", 0);
    startAnimIfKnown(mBack, "Appear", 0);

    MR::startSystemSE("SE_SY_GALAXY_SELECTED");

    PL_LOG_INFO("fileselect",
                "file %d selected (FileConfirm): the items are pushed away, the camera is at the near point and "
                "the Play/Copy/Icon/Erase buttons are up",
                mSelectedItem + 1);
}

void FileSelectHost::leaveConfirm() {
    mPhase = FileSelectPhase::Select;

    if (mField != nullptr) {
        mField->setZShift(0.0f);
    }

    setPanesVisible(mButtons, kOperationPanes, kOperationPaneNum, false);
    mButtonsVisible = false;

    if (nw4r::lyt::Pane* pRoot = findPane(mBack, "RootPane")) {
        pRoot->SetVisible(false);
    }
    if (nw4r::lyt::Pane* pRoot = findPane(mBros, "RootPane")) {
        pRoot->SetVisible(false);
    }

    MR::startSystemSE("SE_SY_GALAXY_DECIDE_CANCEL");

    // Back to the pointed item (or the far point when nothing is pointed).
    mMoveFrom = mCamera;
    mPointedItem = -1;
    mMoveTo = compat::j3d::FileSelectField::cameraFar();
    mMoveStep = 0;

    PL_LOG_INFO("fileselect", "back to the file list (buttons hidden, items back in the fan)");
}

void FileSelectHost::updateConfirmPhase() {
    // The selected item stays at scale 1.0 and the pointer can still hover the
    // buttons (ButtonPaneController::isPointingTrigger on the console).
    if (mHavePointer) {
        if (paneContains(mButtons, "StartButton", mPointerX, mPointerY) ||
            paneContains(mButtons, "BoxStartButton", mPointerX, mPointerY)) {
            MR::startSystemSE("SE_SY_FILE_SEL_UPPER_DECIDE");
            enterPlaying();
            return;
        }

        if (paneContains(mButtons, "CopyButton", mPointerX, mPointerY)) {
            // FileSelector::callbackCopy -> CopyWait/CopySelect. The host copies
            // the save into the first empty slot, which is what the screen shows.
            FileSelectSaveInfo source;

            if (readSave(mSelectedItem + 1, &source)) {
                for (s32 slot = 1; slot <= kSlotCount; ++slot) {
                    if (!hasSave(slot)) {
                        FileSelectSaveInfo copy = source;
                        copy.number = slot;
                        copy.lastModified = static_cast< u64 >(std::time(nullptr));
                        writeSave(copy);
                        refreshFieldFromSaves();
                        PL_LOG_INFO("fileselect", "copy: file %d -> slot %d", mSelectedItem + 1, slot);
                        break;
                    }
                }
            }

            leaveConfirm();
            return;
        }

        if (paneContains(mButtons, "MiiButton", mPointerX, mPointerY)) {
            // FileSelector::callbackMii needs RVLFaceLib (Mii data) — not ported.
            PL_LOG_WARN("fileselect",
                        "Icon (Mii) needs RVLFaceLib, which is not ported yet — the icon stays the character's");
            leaveConfirm();
            return;
        }

        if (paneContains(mButtons, "DeleteButton", mPointerX, mPointerY)) {
            // FileSelector::callbackDelete -> DeleteConfirm (a SysInfoWindow asks
            // for confirmation). The host asks in the log and erases immediately;
            // the confirmation window lands with the message system.
            PL_LOG_INFO("fileselect", "erase confirmed for file %d (the confirmation window is not ported)",
                        mSelectedItem + 1);
            deleteSave(mSelectedItem + 1);
            refreshFieldFromSaves();
            leaveConfirm();
            return;
        }

        if (paneContains(mButtons, "P2ManualButton", mPointerX, mPointerY)) {
            PL_LOG_INFO("fileselect", "2P manual requested (Manual2P is not ported)");
        }
    }

    if (mLastCancel) {
        leaveConfirm();
    }
}

void FileSelectHost::enterPlaying() {
    mPhase = FileSelectPhase::Playing;
    mPlayingSlot = mSelectedItem + 1;

    FileSelectSaveInfo info;

    if (!readSave(mPlayingSlot, &info)) {
        // "New file": FileSelector::exeCreateConfirm creates the file. The host
        // creates the slot with the default character and no stars.
        info.number = mPlayingSlot;
        info.exists = true;
        info.character = 0;
        info.lastModified = static_cast< u64 >(std::time(nullptr));
        std::swprintf(info.name, sizeof(info.name) / sizeof(wchar_t), L"%ls",
                      gameTextOr("System_FileSelect_Icon000", L"Mario"));
        writeSave(info);
    }

    // FileSelector::exeFileSelect: SE_SY_FILE_SELECTED + stopStageBGM(90), then
    // the game scene takes over. The host stops the BGM and reports; the
    // GameScene port is M10.2.
    MR::startSystemSE("SE_SY_FILE_SELECTED");
    MR::stopStageBGM(90);
    mBgmStarted = false;

    PL_LOG_INFO("fileselect",
                "Play This File: slot %d — stopping the file-select BGM and handing over to the game "
                "(the GameScene is not ported yet, so the screen stays here)",
                mPlayingSlot);
}

// -----------------------------------------------------------------------------
// Drawing
// -----------------------------------------------------------------------------
void FileSelectHost::draw3D() {
    if (mField == nullptr || !mField->loaded() || !mField->visible()) {
        return;
    }

    f32 fbWidth = 0.0f;
    f32 fbHeight = 0.0f;
    compat::ui::framebufferSize(&fbWidth, &fbHeight);

    if (fbWidth <= 0.0f || fbHeight <= 0.0f) {
        return;
    }

    const f32 aspect = fbWidth / fbHeight;

    // FileSelectCameraController's programmable camera: makePerspective(fovy,
    // aspect, 100, 800000) with the state's arbitrary look-at.
    Mtx44 proj;
    C_MTXPerspective(proj, mCamera.fovy, aspect, 100.0f, 800000.0f);
    GXSetProjection(proj, GX_PERSPECTIVE);

    Mtx view;
    compat::j3d::FileSelectField::calcViewMtx(mCamera, view);
    mField->draw(view);
}

void FileSelectHost::drawCursor() const {
    if (!mHavePointer) {
        return;
    }

    f32 fbWidth = 0.0f;
    f32 fbHeight = 0.0f;
    compat::ui::framebufferSize(&fbWidth, &fbHeight);

    if (fbWidth <= 0.0f || fbHeight <= 0.0f) {
        return;
    }

    // Ortho in pixels (the same space the title backdrop uses).
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
    C_MTXOrtho(projMtx, 0.0f, fbHeight, 0.0f, fbWidth, -1.0f, 1.0f);
    GXSetProjection(projMtx, GX_ORTHOGRAPHIC);

    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_VTX, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_SET);
    GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GXSetCullMode(GX_CULL_NONE);

    // StarPointer: the gloved hand holding the blue star. The hand points up-left
    // from the pointer's tip, so the palm sits below-right of the hot spot —
    // the reference captures show the star under the fingertip.
    const f32 unit = fbHeight * 0.0016f;  // ~1.7 px at 1080p, scales with the window
    const f32 cx = mPointerX;
    const f32 cy = mPointerY;

    // Wrist / cuff.
    drawQuad(cx - 9.0f * unit, cy + 5.0f * unit, 6.0f * unit, 14.0f * unit, 236, 240, 248, 255);
    // Palm.
    drawQuad(cx - 9.0f * unit, cy + 1.0f * unit, 13.0f * unit, 12.0f * unit, 250, 252, 255, 255);
    // Fingers (the pointing finger plus two folded ones).
    drawQuad(cx - 3.0f * unit, cy - 12.0f * unit, 4.0f * unit, 14.0f * unit, 252, 253, 255, 255);
    drawQuad(cx - 8.0f * unit, cy - 4.0f * unit, 3.5f * unit, 6.0f * unit, 240, 243, 250, 255);
    drawQuad(cx + 1.0f * unit, cy - 4.0f * unit, 3.5f * unit, 6.0f * unit, 240, 243, 250, 255);
    // Blue rim of the glove.
    drawQuad(cx - 9.5f * unit, cy + 12.5f * unit, 13.5f * unit, 1.2f * unit, 40, 90, 190, 220);
    drawQuad(cx - 9.5f * unit, cy + 1.0f * unit, 1.2f * unit, 12.0f * unit, 40, 90, 190, 200);

    // The star in the palm (white outline, blue core) — the StarPointer's
    // "menu" look.
    const f32 starCy = cy + 7.0f * unit;
    const f32 starCx = cx - 3.0f * unit;
    drawStar(starCx, starCy, 6.4f * unit, 2.6f * unit, 255, 255, 255, 255);
    drawStar(starCx, starCy, 4.6f * unit, 1.8f * unit, 66, 150, 245, 255);
    drawDisc(starCx, starCy, 1.4f * unit, 226, 244, 255, 255);
}

bool FileSelectHost::drawGuidance() const {
    // The StarPointer's 1P guidance balloon. On the console the star-pointer
    // actor requests it when the file-select stage comes up
    // (StarPointerUtil.cpp:912, message "System_FileSelect008") and hides it
    // once a file is chosen — capture 1 and 2 show it, capture 3 does not.
    if (mPhase == FileSelectPhase::Playing || mPhase == FileSelectPhase::Leaving) {
        return false;
    }

    if (mPhase == FileSelectPhase::Confirm) {
        return false;
    }

    return compat::ui::drawGuidanceBalloon(
        gameTextOr("System_FileSelect008", L"Please choose a file."));
}

// -----------------------------------------------------------------------------
// Pane helpers
// -----------------------------------------------------------------------------
nw4r::lyt::Pane* FileSelectHost::findPane(const SimpleLayout* pLayout, const char* pPaneName) const {
    if (pLayout == nullptr) {
        return nullptr;
    }

    LayoutManager* pManager = pLayout->getLayoutManager();

    if (pManager == nullptr || pManager->mLayout == nullptr) {
        return nullptr;
    }

    if (nw4r::lyt::Pane* pPane = pManager->findPaneByName(pPaneName)) {
        return pPane;
    }

    return findPaneRecursive(pManager->mLayout->mpRootPane, pPaneName);
}

void FileSelectHost::setPaneTreeVisible(nw4r::lyt::Pane* pPane, bool visible) const {
    if (pPane == nullptr) {
        return;
    }

    pPane->SetVisible(visible);

    for (auto it = pPane->mChildList.GetBeginIter(); it != pPane->mChildList.GetEndIter(); ++it) {
        setPaneTreeVisible(&*it, visible);
    }
}

nw4r::lyt::TextBox* FileSelectHost::findTextBox(const SimpleLayout* pLayout, const char* pPaneName) const {
    nw4r::lyt::Pane* pPane = findPane(pLayout, pPaneName);

    if (pPane != nullptr && pPane->GetRuntimeTypeInfo()->IsDerivedFrom(&nw4r::lyt::TextBox::typeInfo)) {
        return static_cast< nw4r::lyt::TextBox* >(pPane);
    }

    if (pPane == nullptr) {
        PL_LOG_WARN("fileselect", "pane '%s' is not in the layout", pPaneName);
        return nullptr;
    }

    // The pane exists but is a plain pane (or a picture): fall back to the
    // first text box under it, which is where the number/text lives in every
    // SMG layout of this family.
    return firstTextBoxUnder(pPane);
}

void FileSelectHost::setPanesVisible(const SimpleLayout* pLayout, const char* const* pPaneNames, u32 num,
                                     bool visible) {
    if (pLayout == nullptr) {
        return;
    }

    if (pPaneNames == nullptr || num == 0) {
        return;
    }

    for (u32 i = 0; i < num; ++i) {
        if (nw4r::lyt::Pane* pPane = findPane(pLayout, pPaneNames[i])) {
            setPaneTreeVisible(pPane, visible);
        } else {
            PL_LOG_WARN("fileselect", "pane '%s' is not in the layout (nothing to toggle)", pPaneNames[i]);
        }
    }
}

void FileSelectHost::setText(nw4r::lyt::TextBox* pTextBox, const wchar_t* pText) const {
    if (pTextBox == nullptr || pText == nullptr) {
        return;
    }

    pTextBox->SetString(pText, 0);
}

void FileSelectHost::setNumber(nw4r::lyt::TextBox* pTextBox, s32 number) const {
    if (pTextBox == nullptr) {
        return;
    }

    // MR::setTextBoxNumberRecursive's rule (Game/Util/LayoutUtil.cpp): the
    // number is padded with '0' up to the pane's authored digit count and '-' is
    // prepended only when the pane has room for it.
    wchar_t buffer[16];
    std::swprintf(buffer, sizeof(buffer) / sizeof(wchar_t), L"%d", number);
    setText(pTextBox, buffer);
}

bool FileSelectHost::startAnimIfKnown(SimpleLayout* pLayout, const char* pAnimName, u32 layer) const {
    if (pLayout == nullptr) {
        return false;
    }

    LayoutManager* pManager = pLayout->getLayoutManager();

    if (pManager == nullptr) {
        return false;
    }

    // Only start animations the arc really ships: MR::startAnim on an unknown
    // name logs a warning every frame otherwise.
    if (pManager->getAnimTransform(pAnimName) == nullptr) {
        return false;
    }

    MR::startAnim(pLayout, pAnimName, layer);

    return true;
}

// -----------------------------------------------------------------------------
// Hit tests
// -----------------------------------------------------------------------------
s32 FileSelectHost::itemUnderPointer(f32 px, f32 py) const {
    if (mField == nullptr) {
        return -1;
    }

    f32 fbWidth = 0.0f;
    f32 fbHeight = 0.0f;
    compat::ui::framebufferSize(&fbWidth, &fbHeight);

    if (fbWidth <= 0.0f || fbHeight <= 0.0f) {
        return -1;
    }

    // The console's pointing volume (FileSelectItem::initStarPointerTarget):
    // Target(this, 1000.0f, TVec3f(0, 900, 0)) — a cylinder of radius 1000
    // world units centred 900 above the item's centre. project() hands out
    // exactly that radius in screen pixels (radiusPx = 1000 * pxPerUnit), so
    // the hit test is the cylinder's cross-section at the item's depth.
    s32 best = -1;
    f32 bestDepth = 1e30f;

    for (s32 i = 0; i < kSlotCount; ++i) {
        f32 x = 0.0f;
        f32 y = 0.0f;
        f32 z = 0.0f;
        compat::j3d::FileSelectField::calcItemWorldPos(i, mField->zShift(), &x, &y, &z);
        y += 900.0f;

        const compat::j3d::FileSelectItemScreen screen =
            compat::j3d::FileSelectField::project(mCamera, x, y, z, fbWidth, fbHeight);

        if (!screen.visible) {
            continue;
        }

        const f32 dx = px - screen.x;
        const f32 dy = py - screen.y;
        const f32 radius = screen.radiusPx;

        if (dx * dx + dy * dy <= radius * radius && screen.depth < bestDepth) {
            best = i;
            bestDepth = screen.depth;
        }
    }

    return best;
}

bool FileSelectHost::paneRectPixels(const SimpleLayout* pLayout, const char* pPaneName, f32* outX, f32* outY,
                                    f32* outW, f32* outH) const {
    nw4r::lyt::Pane* pPane = findPane(pLayout, pPaneName);

    if (pPane == nullptr) {
        return false;
    }

    // The pane's box in layout units (Y up): the origin is its top-left corner.
    f32 fbWidth = 0.0f;
    f32 fbHeight = 0.0f;
    compat::ui::framebufferSize(&fbWidth, &fbHeight);

    if (fbWidth <= 0.0f || fbHeight <= 0.0f) {
        return false;
    }

    // Pane translations are relative to the parent's top-left, in the layout
    // space SMG uses (Y up); mGlbMtx carries the composed position after the
    // frame's calcAnim. Both corners go through the same layout->pixel mapping
    // the renderer uses.
    f32 ax = 0.0f;
    f32 ay = 0.0f;
    paneAuthoredTranslate(pPane, &ax, &ay);

    const f32 minX = ax;
    const f32 maxY = ay;
    const f32 maxX = ax + pPane->mSize.width;
    const f32 minY = ay - pPane->mSize.height;

    f32 px0 = 0.0f;
    f32 py0 = 0.0f;
    f32 px1 = 0.0f;
    f32 py1 = 0.0f;
    compat::ui::layoutToScreen(minX, minY, fbWidth, fbHeight, &px0, &py0);
    compat::ui::layoutToScreen(maxX, maxY, fbWidth, fbHeight, &px1, &py1);

    if (outX != nullptr) {
        *outX = px0;
    }
    if (outY != nullptr) {
        *outY = py1;  // top edge
    }
    if (outW != nullptr) {
        *outW = px1 - px0;
    }
    if (outH != nullptr) {
        *outH = py0 - py1;
    }

    return true;
}

bool FileSelectHost::paneContains(const SimpleLayout* pLayout, const char* pPaneName, f32 px, f32 py) const {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 w = 0.0f;
    f32 h = 0.0f;

    if (!paneRectPixels(pLayout, pPaneName, &x, &y, &w, &h)) {
        return false;
    }

    return px >= x && px <= x + w && py >= y && py <= y + h;
}

bool FileSelectHost::pointerPixel(f32* outX, f32* outY) const {
    if (!mHavePointer) {
        return false;
    }

    if (outX != nullptr) {
        *outX = mPointerX;
    }
    if (outY != nullptr) {
        *outY = mPointerY;
    }

    return true;
}

// -----------------------------------------------------------------------------
// The save bar (FileSelector::setFileInfo + FileSelectInfo::setInfo)
// -----------------------------------------------------------------------------
void FileSelectHost::applySaveToInfoBar(s32 slot) {
    if (mInfo == nullptr) {
        return;
    }

    FileSelectSaveInfo info;
    const bool exists = readSave(slot, &info);

    nw4r::lyt::TextBox* pName = findTextBox(mInfo, "FileName");
    nw4r::lyt::TextBox* pDay = findTextBox(mInfo, "TxtDay");
    nw4r::lyt::TextBox* pTime = findTextBox(mInfo, "TxtTime");
    nw4r::lyt::TextBox* pNumber = findTextBox(mInfo, "FileNumber");

    if (!exists) {
        // An empty slot: the console keeps the bar but shows the "new file"
        // wording and no counters (FileSelector::setFileInfo with an empty
        // UserFileInfo).
        setText(pName, gameTextOr("System_FileSelect_NewFile", L"New File"));
        setText(pDay, L"");
        setText(pTime, L"");
        setNumber(findTextBox(mInfo, "Star"), 0);
        setNumber(findTextBox(mInfo, "StarPiece"), 0);
    } else {
        wchar_t date[32];
        wchar_t time[16];
        const std::time_t stamp = static_cast< std::time_t >(info.lastModified);
        std::tm* pLocal = std::localtime(&stamp);

        // The formats are the game's own messages (System_Date000 /
        // System_Time002), so the bar prints the same date style the console
        // does in every language (dd/mm/yyyy in the European ones, yyyy/mm/dd
        // in Japanese, ...).
        const wchar_t* pDateFormat = gameTextOr("System_Date000", L"%02d/%02d/%04d");
        const wchar_t* pTimeFormat = gameTextOr("System_Time002", L"%02d:%02d");

        if (pLocal != nullptr) {
            std::swprintf(date, sizeof(date) / sizeof(wchar_t), pDateFormat, pLocal->tm_mday,
                          pLocal->tm_mon + 1, pLocal->tm_year + 1900);
            std::swprintf(time, sizeof(time) / sizeof(wchar_t), pTimeFormat, pLocal->tm_hour, pLocal->tm_min);
        } else {
            date[0] = L'\0';
            time[0] = L'\0';
        }

        setText(pName, info.name[0] != L'\0' ? info.name : gameTextOr("System_FileSelect_Icon000", L"Mario"));
        setText(pDay, date);
        setText(pTime, time);
        setNumber(findTextBox(mInfo, "Star"), info.powerStars);
        setNumber(findTextBox(mInfo, "StarPiece"), info.starPieces);
    }

    setNumber(pNumber, slot);

    if (!mInfoVisible) {
        // FileSelectInfo::appear -> the "Appear" animation on layer 0 and then
        // ButtonAppear on layer 1 (the slide the reference capture shows).
        startAnimIfKnown(mInfo, "Appear", 0);
        startAnimIfKnown(mInfo, "ButtonAppear", 1);
        mInfoVisible = true;
    }

    mInfoSlot = slot;

    PL_LOG_INFO("fileselect", "info bar: slot %d (%s)", slot, exists ? "file data" : "empty slot");
}

}  // namespace compat::game
