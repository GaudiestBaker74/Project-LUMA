// =============================================================================
// FileSelect screen tests (PC_PORT M10/M10.1).
//
// The acceptance criteria of the file-select screen are structural, so they are
// pinned here instead of by eyeballing a capture:
//
//   1. the SAVE STORE round-trips (write → read → erase) and the v1 marker the
//      M10 stand-in wrote is still readable (an empty Mario file);
//   2. the GUIDANCE BALLOON geometry reproduces the reference capture
//      (docs/images/fileselect/1_seleccion_save.png: bar 1165 x 97 px, bottom edge y = 990 px at
//      1920x1080 — i.e. 491.9 x 41 design units, centred);
//   3. the TEXT is ONE language: every FileSelect string follows
//      compat::setLanguage, and the pane-name language suffix resolves to the
//      same message id;
//   4. the 3D FIELD is symmetric and its camera states are the real
//      FileSelectCameraController ones (far / title / near + the 60-frame eased
//      move), with the badge sitting above its planet;
//   5. the SCREEN's phase machine shows the operation buttons only once a file
//      is selected — capture 1 (nothing pointed) has no buttons, capture 3
//      (file selected) has all of them.
//
// The layout/model assets are not present in the test environment: every mount
// degrades (the layout manager tolerates a missing arc, the field reports
// loaded() == false) and these tests only exercise what does not need them.
// =============================================================================

#include "compat/game/FileSelectHost.h"
#include "compat/game/GameTextTable.h"
#include "compat/game/LanguageCompat.h"
#include "compat/game/UiAnchoring.h"
#include "compat/j3d/FileSelectField.h"
#include "compat/ui/GuidanceBanner.h"

#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>

#include "test_runner.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

using compat::game::FileSelectHost;
using compat::game::FileSelectSaveInfo;
using compat::j3d::FileSelectCamera;
using compat::j3d::FileSelectField;

namespace {

bool nearlyEqual(f32 a, f32 b, f32 epsilon = 1.0e-3f) {
    return std::fabs(a - b) <= epsilon;
}

void ensureHeap() {
    if (JKRHeap::sRootHeap == nullptr) {
        JKRExpHeap::createRoot(1, true);
    }

    JKRHeap::sRootHeap->becomeCurrentHeap();
}

/// The save store lives in the current working directory (saves/slotN.bin): the
/// tests move into their own directory so they cannot touch a player's saves.
/// Returns the path to restore afterwards.
std::string enterTempDir(const char* pLeaf) {
    std::error_code ec;
    const std::string previous = std::filesystem::current_path(ec).string();
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path(ec) / "luma_fileselect_test" / pLeaf;

    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    std::filesystem::current_path(dir, ec);

    return previous;
}

void leaveTempDir(const std::string& previous) {
    std::error_code ec;
    std::filesystem::current_path(previous, ec);
}

}  // namespace

// -----------------------------------------------------------------------------
// 1. The save store
// -----------------------------------------------------------------------------
TEST_CASE(fileselect_save_round_trip_and_erase) {
    ensureHeap();

    const std::string previous = enterTempDir("roundtrip");

    FileSelectSaveInfo info;
    info.number = 3;
    info.exists = true;
    info.character = 1;  // Luigi
    info.powerStars = 18;
    info.starPieces = 449;
    info.lastModified = 1600970000;  // 23/09/2020 (the reference capture's file)
    info.clearedNormal = true;

    std::swprintf(info.name, 16, L"Luigi");

    CHECK(FileSelectHost::writeSave(info));
    CHECK(FileSelectHost::hasSave(3));
    CHECK(!FileSelectHost::hasSave(4));

    FileSelectSaveInfo read;
    REQUIRE(FileSelectHost::readSave(3, &read));

    CHECK_EQ(read.number, 3);
    CHECK_EQ(read.character, 1);
    CHECK_EQ(read.powerStars, 18);
    CHECK_EQ(read.starPieces, 449);
    CHECK_EQ(static_cast< long long >(read.lastModified), 1600970000LL);
    CHECK(read.clearedNormal);
    CHECK(!read.clearedComplete);
    CHECK(std::wcscmp(read.name, L"Luigi") == 0);

    CHECK(FileSelectHost::deleteSave(3));
    CHECK(!FileSelectHost::hasSave(3));

    leaveTempDir(previous);
}

TEST_CASE(fileselect_v1_marker_reads_as_an_empty_mario_file) {
    ensureHeap();

    // The M10 stand-in wrote magic + version 1 + slot + timestamp: a player who
    // ran that build must not lose the screen (the bar shows an empty file).
    const std::string previous = enterTempDir("v1");

    std::error_code ec;
    std::filesystem::create_directories("saves", ec);

    FILE* fp = std::fopen("saves/slot2.bin", "wb");
    REQUIRE(fp != nullptr);

    const char magic[8] = {'L', 'U', 'M', 'A', 'S', 'A', 'V', 'E'};
    const unsigned version = 1;
    const unsigned slot = 2;
    const unsigned long long stamp = 1600970000ULL;
    std::fwrite(magic, 1, sizeof(magic), fp);
    std::fwrite(&version, 1, sizeof(version), fp);
    std::fwrite(&slot, 1, sizeof(slot), fp);
    std::fwrite(&stamp, 1, sizeof(stamp), fp);
    std::fclose(fp);

    FileSelectSaveInfo info;
    REQUIRE(FileSelectHost::readSave(2, &info));

    CHECK(info.exists);
    CHECK_EQ(info.number, 2);
    CHECK_EQ(info.powerStars, 0);
    CHECK_EQ(info.starPieces, 0);
    CHECK(std::wcscmp(info.name, L"Mario") == 0);
    CHECK_EQ(static_cast< long long >(info.lastModified), 1600970000LL);

    leaveTempDir(previous);
}

// -----------------------------------------------------------------------------
// 2. The guidance balloon ("Please choose a file.")
// -----------------------------------------------------------------------------
TEST_CASE(fileselect_guidance_balloon_matches_the_reference_capture) {
    // Reference measurement: docs/images/fileselect/1_seleccion_save.png (1920x1080) shows the bar
    // as 1165 x 97 px with its bottom edge at y = 990 and its centre on the
    // screen's vertical axis. In design units that is 491.9 x 41.0 with the
    // bottom edge at y = -190, and the English text measures 256.3 units — the
    // number the balloon's padding is derived from.
    constexpr f32 kFbWidth = 1920.0f;
    constexpr f32 kFbHeight = 1080.0f;
    constexpr f32 kReferenceTextWidthUnits = 256.3f;

    const f32 scale = compat::ui::uiScale(kFbWidth, kFbHeight);

    CHECK(nearlyEqual(scale, 1080.0f / 456.0f));

    const compat::ui::GuidanceBalloonLayout layout =
        compat::ui::layoutGuidanceBalloon(kReferenceTextWidthUnits, scale, kFbWidth, kFbHeight);

    CHECK(nearlyEqual(layout.barWidth, 1165.0f, 2.0f));
    CHECK(nearlyEqual(layout.barHeight, 97.0f, 1.0f));
    CHECK(nearlyEqual(layout.barLeft, 377.0f, 2.0f));
    CHECK(nearlyEqual(layout.barTop + layout.barHeight, 990.0f, 2.0f));
    CHECK(nearlyEqual(layout.cornerRadius, layout.barHeight * 0.5f));
    CHECK(nearlyEqual(layout.fontSize, 26.0f * scale));
    // The text is centred on the same axis as the bar.
    CHECK(nearlyEqual(layout.textLeft + layout.textWidth * 0.5f, kFbWidth * 0.5f));
    CHECK(nearlyEqual(layout.textTop, layout.barTop + (layout.barHeight - layout.fontSize) * 0.5f));

    // A longer localized string widens the bar symmetrically (the console's
    // guidance window grows with its message) and a shorter one narrows it.
    const compat::ui::GuidanceBalloonLayout wider =
        compat::ui::layoutGuidanceBalloon(kReferenceTextWidthUnits + 100.0f, scale, kFbWidth, kFbHeight);
    CHECK(wider.barWidth > layout.barWidth);
    CHECK(nearlyEqual(wider.barLeft + wider.barWidth * 0.5f, kFbWidth * 0.5f));
    CHECK(nearlyEqual(wider.barTop + wider.barHeight, 990.0f, 2.0f));
}

// -----------------------------------------------------------------------------
// 3. One language per screen
// -----------------------------------------------------------------------------
TEST_CASE(fileselect_strings_follow_the_selected_language) {
    // The M10.1 bug: the screen showed the arc's authored Japanese panes next to
    // another language's button string. Every string the screen renders has to
    // come from the SAME language column.
    struct LanguageExpectation {
        const char* folder;
        const wchar_t* startButton;  // Layout_FileSelectTxtStart
        const wchar_t* copy;         // Layout_FileSelectTxtCopy
        const wchar_t* erase;        // Layout_FileSelectTxtDelete
        const wchar_t* guidance;     // System_FileSelect008
    };

    const LanguageExpectation expectations[] = {
        {"UsEnglish", L"Play This File", L"Copy", L"Erase", L"Please choose a file."},
        {"UsSpanish", L"Juega con estos datos", L"Copiar", L"Borrar", L"Elige un archivo."},
        {"UsFrench", L"Jouer avec ces données", L"Copier", L"Effacer", L"Choisis un fichier."},
        {"EuGerman", L"Mit diesen Daten spielen", L"Kopieren", L"Löschen", L"Bitte wähle eine Datei."},
        {"JpJapanese", L"このデータで遊ぶ", L"コピー", L"けす", L"ファイルをえらんでください"},
    };

    for (const LanguageExpectation& e : expectations) {
        REQUIRE(compat::setLanguage(e.folder));

        const wchar_t* pStart = compat::game::gameTextForMessageId("Layout_FileSelectTxtStart");
        REQUIRE(pStart != nullptr);
        CHECK(std::wcscmp(pStart, e.startButton) == 0);

        const wchar_t* pCopy = compat::game::gameTextForMessageId("Layout_FileSelectTxtCopy");
        REQUIRE(pCopy != nullptr);
        CHECK(std::wcscmp(pCopy, e.copy) == 0);

        const wchar_t* pErase = compat::game::gameTextForMessageId("Layout_FileSelectTxtDelete");
        REQUIRE(pErase != nullptr);
        CHECK(std::wcscmp(pErase, e.erase) == 0);

        const wchar_t* pGuidance = compat::game::gameTextForMessageId("System_FileSelect008");
        REQUIRE(pGuidance != nullptr);
        CHECK(std::wcscmp(pGuidance, e.guidance) == 0);

        // The Back button of the same language (BackButton.arc).
        const wchar_t* pBack = compat::game::gameTextForMessageId("Layout_BackButtonBack");
        REQUIRE(pBack != nullptr);
        CHECK(pBack[0] != L'\0');
    }

    compat::setLanguage("UsEnglish");
}

TEST_CASE(fileselect_pane_suffix_resolves_to_the_same_message) {
    // The arc carries one text box per language ("TxtStartUsEn", "ShaStartJpJa",
    // ...) inside the SAME layout; the message id rule strips the suffix, so all
    // of them resolve to the string of the language currently selected — that is
    // what makes the variant panes agree instead of mixing Japanese and French.
    REQUIRE(compat::setLanguage("EuGerman"));

    char id[128];
    compat::game::buildLayoutMessageId(id, sizeof(id), "FileSelect", "TxtStartUsEn");
    CHECK(std::strcmp(id, "Layout_FileSelectTxtStart") == 0);

    compat::game::buildLayoutMessageId(id, sizeof(id), "FileSelect", "ShaStartJpJa");
    CHECK(std::strcmp(id, "Layout_FileSelectShaStart") == 0);

    const wchar_t* pVariant = compat::game::gameTextForMessageId(id);
    REQUIRE(pVariant != nullptr);
    CHECK(std::wcscmp(pVariant, L"Mit diesen Daten spielen") == 0);

    // BackButton's pane is "TxtBack" (its layout name prefixes the id).
    compat::game::buildLayoutMessageId(id, sizeof(id), "BackButton", "TxtBack");
    CHECK(std::strcmp(id, "Layout_BackButtonTxtBack") == 0);

    CHECK(!compat::game::hasGameText("Layout_FileSelectNoSuchPane"));

    compat::setLanguage("UsEnglish");
}

// -----------------------------------------------------------------------------
// 4. The 3D field and the camera
// -----------------------------------------------------------------------------
TEST_CASE(fileselect_field_placement_is_symmetric_under_the_planet_heads) {
    // The reference capture shows the six planets in a V: 1/2 in front, then
    // 3/4, then 5/6 — symmetric about the vertical axis, with Mario's head
    // between 1 and 2.
    CHECK_EQ(FileSelectField::placement(0).number, 1);
    CHECK_EQ(FileSelectField::placement(1).number, 2);
    CHECK_EQ(FileSelectField::placement(5).number, 6);

    for (int i = 0; i < FileSelectField::kItemNum; i += 2) {
        const compat::j3d::FileSelectItemPlacement& left = FileSelectField::placement(i);
        const compat::j3d::FileSelectItemPlacement& right = FileSelectField::placement(i + 1);

        // Mirrored pair: x is opposite, y and z identical.
        CHECK(nearlyEqual(left.x, -right.x, 1.0f));
        CHECK(nearlyEqual(left.y, right.y, 1.0f));
        CHECK(nearlyEqual(left.z, right.z, 1.0f));
    }

    // The fan rises and recedes: 1/2 in front, 5/6 behind them.
    CHECK(FileSelectField::placement(4).y > FileSelectField::placement(2).y);
    CHECK(FileSelectField::placement(2).y > FileSelectField::placement(0).y);
    CHECK(FileSelectField::placement(4).z < FileSelectField::placement(0).z);

    // The badge floats ABOVE its planet (FileSelectNumber is placed from the
    // item's 3D position).
    f32 itemX = 0.0f;
    f32 itemY = 0.0f;
    f32 itemZ = 0.0f;
    f32 badgeX = 0.0f;
    f32 badgeY = 0.0f;
    f32 badgeZ = 0.0f;
    FileSelectField::calcItemWorldPos(0, 0.0f, &itemX, &itemY, &itemZ);
    FileSelectField::calcBadgeWorldPos(0, 0.0f, &badgeX, &badgeY, &badgeZ);

    CHECK(nearlyEqual(badgeX, itemX));
    CHECK(nearlyEqual(badgeZ, itemZ));
    CHECK(badgeY > itemY + 500.0f);

    // calcBasePos(dz) shifts the whole fan (FileSelector::goToNearPoint uses
    // -16000 when a file is selected).
    FileSelectField::calcItemWorldPos(0, -16000.0f, &itemX, &itemY, &itemZ);
    CHECK(nearlyEqual(itemZ, -16000.0f));
}

TEST_CASE(fileselect_camera_states_are_the_controller_constants) {
    const FileSelectCamera far = FileSelectField::cameraFar();
    CHECK(nearlyEqual(far.pos[0], 0.0f));
    CHECK(nearlyEqual(far.pos[1], 0.0f));
    CHECK(nearlyEqual(far.pos[2], 15000.0f));
    CHECK(nearlyEqual(far.target[1], 800.0f));
    CHECK(nearlyEqual(far.fovy, 40.0f));

    // Title state = the far state lifted 15000: the TARGET moves with
    // cFarTarget.y and the camera with cFarPoint.y (decomp lines 79-80).
    const FileSelectCamera title = FileSelectField::cameraTitle();
    CHECK(nearlyEqual(title.pos[1], 15000.0f));
    CHECK(nearlyEqual(title.pos[2], 15000.0f));
    CHECK(nearlyEqual(title.target[1], 15800.0f));
    CHECK(nearlyEqual(title.target[2], 0.0f));
    CHECK(nearlyEqual(title.fovy, 60.0f));

    // Near state: the camera sits 4800 units in front of a point 1100 above the
    // item (cNearPointOffset / cNearTargetOffset), fovy 50.
    const FileSelectCamera near = FileSelectField::cameraNear(-2400.0f, -300.0f, 0.0f);
    CHECK(nearlyEqual(near.target[0], -2400.0f));
    CHECK(nearlyEqual(near.target[1], 800.0f));
    CHECK(nearlyEqual(near.pos[2], 4800.0f));
    CHECK(nearlyEqual(near.fovy, 50.0f));

    // The move is 60 frames of squared time (exeMoveTo*Point).
    const FileSelectCamera half = FileSelectField::blendCamera(far, near, 0.5f);
    CHECK(nearlyEqual(half.pos[2], far.pos[2] + (near.pos[2] - far.pos[2]) * 0.25f));
    CHECK(nearlyEqual(half.fovy, far.fovy + (near.fovy - far.fovy) * 0.25f));

    const FileSelectCamera start = FileSelectField::blendCamera(far, near, 0.0f);
    CHECK(nearlyEqual(start.pos[2], far.pos[2]));
    const FileSelectCamera end = FileSelectField::blendCamera(far, near, 1.0f);
    CHECK(nearlyEqual(end.pos[2], near.pos[2]));

    // Projection: the look-at target lands on the framebuffer centre, and an
    // item twice as far away covers half the pixels.
    const compat::j3d::FileSelectItemScreen centre =
        FileSelectField::project(far, far.target[0], far.target[1], far.target[2], 1920.0f, 1080.0f);
    CHECK(centre.visible);
    CHECK(nearlyEqual(centre.x, 960.0f, 0.5f));
    CHECK(nearlyEqual(centre.y, 540.0f, 0.5f));

    const compat::j3d::FileSelectItemScreen halfDistance =
        FileSelectField::project(far, 0.0f, 800.0f, 7500.0f, 1920.0f, 1080.0f);
    CHECK(halfDistance.visible);
    CHECK(halfDistance.radiusPx > centre.radiusPx * 1.9f);

    // What the reference capture shows: with the far camera and the framebuffer
    // above, the badge of item 1 lands LEFT of the centre and item 2 right of it
    // (the V of the six planets).
    f32 badgeX = 0.0f;
    f32 badgeY = 0.0f;
    f32 badgeZ = 0.0f;
    FileSelectField::calcBadgeWorldPos(0, 0.0f, &badgeX, &badgeY, &badgeZ);
    const compat::j3d::FileSelectItemScreen left =
        FileSelectField::project(far, badgeX, badgeY, badgeZ, 1920.0f, 1080.0f);
    FileSelectField::calcBadgeWorldPos(1, 0.0f, &badgeX, &badgeY, &badgeZ);
    const compat::j3d::FileSelectItemScreen right =
        FileSelectField::project(far, badgeX, badgeY, badgeZ, 1920.0f, 1080.0f);

    CHECK(left.visible && right.visible);
    CHECK(left.x < 960.0f);
    CHECK(right.x > 960.0f);
    CHECK(nearlyEqual(left.x - 960.0f, 960.0f - right.x, 4.0f));

    // And they land where the capture measured them. The badge numerals of
    // docs/images/fileselect/1_seleccion_save.png (1920x1080) are at design units
    //   (-101.1, -4.1) / (100.6, -5.3)   <- items 1 and 2
    // and the table in FileSelectField.cpp was solved from exactly that, so the
    // projection has to come back to it (1 design unit = 2.37 px = ~2 px of
    // slack here).
    const f32 designPerPixel = 456.0f / 1080.0f;
    CHECK(nearlyEqual((left.x - 960.0f) * designPerPixel, -101.1f, 3.0f));
    CHECK(nearlyEqual((960.0f - left.y) * designPerPixel, -4.1f, 3.0f));
    CHECK(nearlyEqual((right.x - 960.0f) * designPerPixel, 100.6f, 3.0f));
    CHECK(nearlyEqual((960.0f - right.y) * designPerPixel, -5.3f, 3.0f));

    // Item 5 (the pair furthest up the V) has to project ABOVE item 1.
    FileSelectField::calcBadgeWorldPos(4, 0.0f, &badgeX, &badgeY, &badgeZ);
    const compat::j3d::FileSelectItemScreen back =
        FileSelectField::project(far, badgeX, badgeY, badgeZ, 1920.0f, 1080.0f);
    CHECK(back.visible);
    CHECK(back.y < left.y);
}

// -----------------------------------------------------------------------------
// 5. The screen's phase machine
// -----------------------------------------------------------------------------
TEST_CASE(fileselect_screen_starts_in_the_appear_phase_without_buttons) {
    ensureHeap();

    const std::string previous = enterTempDir("phases");

    FileSelectHost host;
    host.init();

    // Capture 1: the items fade in and NOTHING is selected — no info bar, no
    // operation buttons, no player badge.
    CHECK(host.phase() == compat::game::FileSelectPhase::Appear);
    CHECK_EQ(host.pointedItem(), -1);
    CHECK_EQ(host.selectedItem(), -1);
    CHECK_EQ(host.playingSlot(), -1);
    CHECK(!host.playerBadgeVisible());

    // The camera flies from the title point (fovy 60) to the far point (fovy 40)
    // over the 60 frames of the move.
    CHECK(nearlyEqual(host.cameraFovy(), 60.0f, 0.5f));

    for (int i = 0; i < 46; ++i) {
        host.update();
    }

    CHECK(host.phase() == compat::game::FileSelectPhase::Select);
    CHECK(nearlyEqual(host.cameraFovy(), 40.0f, 0.6f));

    // Nothing is pointed at: no badge, and the guidance balloon is the only 2D
    // element of the screen. Drawing it needs the boot's message font, which a
    // headless run does not have — what is asserted here is that the call is
    // safe without one (it reports "not drawn" instead of faulting).
    CHECK(!host.playerBadgeVisible());
    CHECK(!host.drawGuidance());

    leaveTempDir(previous);
}
