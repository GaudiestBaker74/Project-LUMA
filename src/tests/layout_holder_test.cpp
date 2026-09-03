// =============================================================================
// M9.5.3a: layout stack tests (headless, synthetic data only).
//
// Exercises the reconstructed pieces of the real layout pipeline:
//
//   - LayoutHolder (patched copy): mounts a synthetic RARC built in-memory
//     (same builder pattern as jkr_archive_test.cpp) whose root holds a
//     .brlyt, a .brlan, a .tpl and a .brfnt, and verifies the three
//     ResTables classify them by extension with stripped names, that
//     GetResource('timg'/'font', ...) serves the blobs (with and without the
//     extension in the query) and that GetFont reports the not-yet-wired
//     global font holder with null.
//   - LayoutAnmPlayer (patched copy): isStop() is true while no transform is
//     bound (the M9.5.3b window), and movement()/reflectFrame() are safe.
//   - J3DFrameCtrl (extracted verbatim into jsystem/J3DFrameCtrlCompat.cpp):
//     a non-looping controller flags the stop state bit at the end frame —
//     exactly what LayoutAnmPlayer::isStop() reports to the nerves — and a
//     looping one never does.
// =============================================================================

#include "tests/test_runner.h"

#include "Game/Animation/LayoutAnmPlayer.hpp"
#include "Game/System/LayoutHolder.hpp"

#include <JSystem/J3DGraphAnimator/J3DAnimation.hpp>
#include <JSystem/JKernel/JKRArchive.hpp>
#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#include <JSystem/JKernel/JKRMemArchive.hpp>

#include <cctype>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Big-endian writers + CArcName hash (copied from jkr_archive_test.cpp; these
// helpers live in anonymous namespaces per translation unit by design).
// ---------------------------------------------------------------------------

void put16(std::vector<u8>& v, u16 value) {
    v.push_back(static_cast<u8>(value >> 8));
    v.push_back(static_cast<u8>(value));
}

void put32(std::vector<u8>& v, u32 value) {
    v.push_back(static_cast<u8>(value >> 24));
    v.push_back(static_cast<u8>((value >> 16) & 0xFF));
    v.push_back(static_cast<u8>((value >> 8) & 0xFF));
    v.push_back(static_cast<u8>(value & 0xFF));
}

void patch32(std::vector<u8>& v, size_t offset, u32 value) {
    v[offset + 0] = static_cast<u8>(value >> 24);
    v[offset + 1] = static_cast<u8>((value >> 16) & 0xFF);
    v[offset + 2] = static_cast<u8>((value >> 8) & 0xFF);
    v[offset + 3] = static_cast<u8>(value & 0xFF);
}

u16 arcHash(const char* name) {
    u16 hash = 0;
    for (const char* p = name; *p != '\0'; ++p) {
        hash = static_cast<u16>(static_cast<u16>(tolower(*p)) + hash * 3);
    }
    return hash;
}

JKRHeap* ensureHeap() {
    if (JKRHeap::sRootHeap == nullptr) {
        JKRExpHeap::createRoot(1, true);
    }
    JKRHeap::sRootHeap->becomeCurrentHeap();
    return JKRHeap::sRootHeap;
}

// ---------------------------------------------------------------------------
// Synthetic single-directory RARC: every entry of `files` lands in the root
// dir, 0x20-aligned, all big-endian (same geometry as buildTestRarc in
// jkr_archive_test.cpp).
// ---------------------------------------------------------------------------

std::vector<u8> buildLayoutArcRarc(const std::vector<std::pair<std::string, std::vector<u8>>>& files) {
    // String table: "." followed by every file name.
    std::vector<u8> strings;
    strings.push_back('.');
    strings.push_back('\0');

    std::vector<u32> nameOffsets;
    for (const auto& entry : files) {
        nameOffsets.push_back(static_cast<u32>(strings.size()));
        for (char c : entry.first) {
            strings.push_back(static_cast<u8>(c));
        }
        strings.push_back('\0');
    }

    const u32 nrDirs = 1;
    const u32 nrFiles = static_cast<u32>(files.size());
    const u32 dirOffset = 0x20;  // relative to the info block
    const u32 fileOffset = dirOffset + nrDirs * 0x10;
    const u32 stringTableOffset = fileOffset + nrFiles * 0x14;
    const u32 stringTableSize = static_cast<u32>(strings.size());
    const u32 headerSize = 0x20;
    const u32 tablesEnd = headerSize + 0x20 + stringTableOffset + stringTableSize;
    const u32 fileDataAbs = (tablesEnd + 0x1F) & ~0x1Fu;
    const u32 fileDataOffset = fileDataAbs - headerSize;

    // Per-file data offsets inside the data section (0x20-aligned each).
    std::vector<u32> dataOffsets;
    u32 cursor = 0;
    for (const auto& entry : files) {
        dataOffsets.push_back(cursor);
        cursor = static_cast<u32>((cursor + entry.second.size() + 0x1F) & ~0x1Fu);
    }
    const u32 totalDataSize = cursor;

    std::vector<u8> blob;
    // --- header ---
    blob.push_back('R'); blob.push_back('A'); blob.push_back('R'); blob.push_back('C');
    const size_t fileSizePos = blob.size();
    put32(blob, 0);  // patched at the end
    put32(blob, headerSize);
    put32(blob, fileDataOffset);
    put32(blob, totalDataSize);
    put32(blob, totalDataSize);  // mMRamDataSize (all MRAM)
    put32(blob, 0);              // mARamDataSize
    put32(blob, 0);
    // --- info block ---
    put32(blob, nrDirs);
    put32(blob, dirOffset);
    put32(blob, nrFiles);
    put32(blob, fileOffset);
    put32(blob, stringTableSize);
    put32(blob, stringTableOffset);
    put16(blob, static_cast<u16>(nrFiles));  // mNextAvailableFileID
    put16(blob, 0);                          // mFileIDIsIndex
    put32(blob, 0);
    // --- single root dir entry ---
    put32(blob, 0x524F4F54);  // 'ROOT'
    put32(blob, 0);           // name offset of "."
    put16(blob, 0);
    put16(blob, static_cast<u16>(nrFiles));
    put32(blob, 0);           // first file index
    // --- file entries (0x14 each) ---
    for (u32 i = 0; i < nrFiles; i++) {
        put16(blob, static_cast<u16>(i));
        put16(blob, arcHash(files[i].first.c_str()));
        blob.push_back(0x11);  // file | mram
        blob.push_back(static_cast<u8>((nameOffsets[i] >> 16) & 0xFF));
        put16(blob, static_cast<u16>(nameOffsets[i] & 0xFFFF));
        put32(blob, dataOffsets[i]);
        put32(blob, static_cast<u32>(files[i].second.size()));
        put32(blob, 0);  // padding (console runtime: mFileData)
    }
    // --- string table + padding ---
    blob.insert(blob.end(), strings.begin(), strings.end());
    while (blob.size() < fileDataAbs) {
        blob.push_back(0);
    }
    // --- file data ---
    for (const auto& entry : files) {
        blob.insert(blob.end(), entry.second.begin(), entry.second.end());
        while (blob.size() & 0x1F) {
            blob.push_back(0);
        }
    }

    patch32(blob, fileSizePos, static_cast<u32>(blob.size()));
    return blob;
}

// Payload stand-ins: LayoutHolder only classifies and serves blobs (brlyt
// conversion/parsing happens later in LayoutManager/Layout::Build).
const std::vector<u8> kBrlytBytes{'R', 'L', 'Y', 'T', 0, 1, 2, 3};
const std::vector<u8> kBrlanBytes{'R', 'L', 'A', 'N', 4, 5, 6, 7};
const std::vector<u8> kTplBytes{0x00, 0x20, 0xAF, 0x30, 8, 9};
const std::vector<u8> kBrfntBytes{'R', 'F', 'N', 'T', 10, 11};

// ---------------------------------------------------------------------------

TEST_CASE(layout_holder_classifies_arc_resources) {
    JKRHeap* heap = ensureHeap();
    REQUIRE(heap != nullptr);

    // Real RARC string tables hold lower-case names (the archive lookup
    // lower-cases the query and strcmps against the table), so the synthetic
    // arc uses lower-case names too. The ResTable lookups below deliberately
    // query with mixed case: MR::getHashCodeLower normalizes both sides.
    const std::vector<u8> rarc = buildLayoutArcRarc({
        {"titlelogo.brlyt", kBrlytBytes},
        {"appear.brlan", kBrlanBytes},
        {"logo.tpl", kTplBytes},
        {"messagefont26.brfnt", kBrfntBytes},
    });

    u8* blob = static_cast<u8*>(JKRHeap::alloc(static_cast<u32>(rarc.size()), 32, heap));
    REQUIRE(blob != nullptr);
    std::memcpy(blob, rarc.data(), rarc.size());

    JKRMemArchive archive;
    REQUIRE(archive.mountFixed(blob, JKR_MEM_BREAK_FLAG_1));
    CHECK(archive.mIsMounted);

    LayoutHolder holder(archive);

    // Extension-based classification, names stored with the extension
    // stripped.
    REQUIRE(holder.mLayoutRes.mCount == 1u);
    CHECK(holder.mLayoutRes.getRes("TitleLogo") != nullptr);
    CHECK(std::strcmp(holder.mLayoutRes.getResName(0u), "titlelogo") == 0);

    REQUIRE(holder.mAnimRes.mCount == 1u);
    CHECK(holder.mAnimRes.getRes("Appear") != nullptr);

    REQUIRE(holder.mResOther.mCount == 2u);
    CHECK(holder.getResOther("logo") != nullptr);
    CHECK(holder.getResOther("MessageFont26") != nullptr);
    CHECK(holder.isExistResOther("logo"));
    CHECK(!holder.isExistResOther("nope"));
    CHECK_EQ(holder.getResOtherNum(), 2u);

    // The blobs served are the archive payloads verbatim (conversion is lazy,
    // downstream in TexMap/ResFont — M9.5.2 hosts).
    void* tpl = holder.GetResource('timg', "logo", nullptr);
    REQUIRE(tpl != nullptr);
    CHECK(std::memcmp(tpl, kTplBytes.data(), kTplBytes.size()) == 0);
    CHECK_EQ(archive.getResSize(tpl), static_cast<s32>(kTplBytes.size()));

    // Query WITH the extension also hits (GetResource strips it as a
    // fallback — brlyt txl1/fnl1 names may carry extensions).
    CHECK(holder.GetResource('timg', "logo.tpl", nullptr) == tpl);
    CHECK(holder.GetResource('font', "MessageFont26", nullptr) != nullptr);
    CHECK(holder.GetResource('timg', "missing", nullptr) == nullptr);

    // Global font holder is not wired until M9.5.3c -> null, no crash.
    CHECK(holder.GetFont("MessageFont26") == nullptr);
}

// ---------------------------------------------------------------------------
// Synthetic multi-directory RARC shaped like the real SMG layout arcs: the
// root node only lists child folders and the files live under anm/, blyt/
// and timg/. Each node's entry list starts with "." and ".." dir entries, so
// the flat table holds 14 entries while only 3 of them are files: iterating
// bounded by countResource() (3) sees nothing but the root's ".", ".." and
// "anm" dir entries. Regression test for the truncation + subfolder-path
// lookup bugs that made every real layout arc appear brlyt-less.
// ---------------------------------------------------------------------------

std::vector<u8> buildSubdirLayoutArcRarc() {
    // --- string table ---
    std::vector<u8> strings;

    auto addString = [&strings](const char* pStr) -> u32 {
        u32 off = static_cast<u32>(strings.size());

        for (const char* p = pStr; *p != '\0'; ++p) {
            strings.push_back(static_cast<u8>(*p));
        }

        strings.push_back('\0');
        return off;
    };

    const u32 offRootName = addString(".");
    const u32 offDot = addString(".");
    const u32 offDotDot = addString("..");
    const u32 offAnm = addString("anm");
    const u32 offBlyt = addString("blyt");
    const u32 offTimg = addString("timg");
    const u32 offBrlan = addString("strap.brlan");
    const u32 offBrlyt = addString("wiiremotestrap.brlyt");
    const u32 offTpl = addString("strap_01.tpl");

    const u32 nrDirs = 4;
    const u32 nrEntries = 14;  // flat table size (dirs + "." ".." + files)
    const u32 dirOffset = 0x20;
    const u32 fileOffset = dirOffset + nrDirs * 0x10;
    const u32 stringTableOffset = fileOffset + nrEntries * 0x14;
    const u32 stringTableSize = static_cast<u32>(strings.size());
    const u32 headerSize = 0x20;
    const u32 tablesEnd = headerSize + 0x20 + stringTableOffset + stringTableSize;
    const u32 fileDataAbs = (tablesEnd + 0x1F) & ~0x1Fu;
    const u32 fileDataOffset = fileDataAbs - headerSize;

    // Payload data offsets inside the data section (0x20-aligned each).
    const u32 brlanOff = 0;
    const u32 brlanEnd = (brlanOff + static_cast<u32>(kBrlanBytes.size()) + 0x1F) & ~0x1Fu;
    const u32 brlytOff = brlanEnd;
    const u32 brlytEnd = (brlytOff + static_cast<u32>(kBrlytBytes.size()) + 0x1F) & ~0x1Fu;
    const u32 tplOff = brlytEnd;
    const u32 totalDataSize = (tplOff + static_cast<u32>(kTplBytes.size()) + 0x1F) & ~0x1Fu;

    std::vector<u8> blob;

    auto putDirEntry = [&blob](const char* pName, u32 nameOff, u32 nodeIdx) {
        put16(blob, 0xFFFF);
        put16(blob, arcHash(pName));
        blob.push_back(0x02);  // folder
        blob.push_back(static_cast<u8>((nameOff >> 16) & 0xFF));
        put16(blob, static_cast<u16>(nameOff & 0xFFFF));
        put32(blob, nodeIdx);  // mDataOffset = node index
        put32(blob, 0x10);
        put32(blob, 0);
    };

    auto putFileEntry = [&blob](u16 fileId, const char* pName, u32 nameOff, u32 dataOff, u32 size) {
        put16(blob, fileId);
        put16(blob, arcHash(pName));
        blob.push_back(0x11);  // file | mram
        blob.push_back(static_cast<u8>((nameOff >> 16) & 0xFF));
        put16(blob, static_cast<u16>(nameOff & 0xFFFF));
        put32(blob, dataOff);
        put32(blob, size);
        put32(blob, 0);
    };

    // --- header ---
    blob.push_back('R'); blob.push_back('A'); blob.push_back('R'); blob.push_back('C');
    const size_t fileSizePos = blob.size();
    put32(blob, 0);  // patched at the end
    put32(blob, headerSize);
    put32(blob, fileDataOffset);
    put32(blob, totalDataSize);
    put32(blob, totalDataSize);
    put32(blob, 0);
    put32(blob, 0);
    // --- info block ---
    put32(blob, nrDirs);
    put32(blob, dirOffset);
    put32(blob, nrEntries);
    put32(blob, fileOffset);
    put32(blob, stringTableSize);
    put32(blob, stringTableOffset);
    put16(blob, 3);  // mNextAvailableFileID (3 files)
    put16(blob, 0);
    put32(blob, 0);
    // --- node table: root(5 entries), anm(3), blyt(3), timg(3) ---
    put32(blob, 0x524F4F54); put32(blob, offRootName); put16(blob, 0); put16(blob, 5); put32(blob, 0);
    put32(blob, 0); put32(blob, offAnm); put16(blob, 0); put16(blob, 3); put32(blob, 5);
    put32(blob, 0); put32(blob, offBlyt); put16(blob, 0); put16(blob, 3); put32(blob, 8);
    put32(blob, 0); put32(blob, offTimg); put16(blob, 0); put16(blob, 3); put32(blob, 11);
    // --- flat entry table ---
    // node 0 (root)
    putDirEntry(".", offDot, 0);
    putDirEntry("..", offDotDot, 0);
    putDirEntry("anm", offAnm, 1);
    putDirEntry("blyt", offBlyt, 2);
    putDirEntry("timg", offTimg, 3);
    // node 1 (anm)
    putDirEntry(".", offDot, 1);
    putDirEntry("..", offDotDot, 0);
    putFileEntry(0, "strap.brlan", offBrlan, brlanOff, static_cast<u32>(kBrlanBytes.size()));
    // node 2 (blyt)
    putDirEntry(".", offDot, 2);
    putDirEntry("..", offDotDot, 0);
    putFileEntry(1, "wiiremotestrap.brlyt", offBrlyt, brlytOff, static_cast<u32>(kBrlytBytes.size()));
    // node 3 (timg)
    putDirEntry(".", offDot, 3);
    putDirEntry("..", offDotDot, 0);
    putFileEntry(2, "strap_01.tpl", offTpl, tplOff, static_cast<u32>(kTplBytes.size()));
    // --- string table + padding ---
    blob.insert(blob.end(), strings.begin(), strings.end());

    while (blob.size() < fileDataAbs) {
        blob.push_back(0);
    }

    // --- file data ---
    blob.insert(blob.end(), kBrlanBytes.begin(), kBrlanBytes.end());

    while (blob.size() < fileDataAbs + brlytOff) {
        blob.push_back(0);
    }

    blob.insert(blob.end(), kBrlytBytes.begin(), kBrlytBytes.end());

    while (blob.size() < fileDataAbs + tplOff) {
        blob.push_back(0);
    }

    blob.insert(blob.end(), kTplBytes.begin(), kTplBytes.end());

    patch32(blob, fileSizePos, static_cast<u32>(blob.size()));
    return blob;
}

TEST_CASE(layout_holder_reads_subfolder_arc_entries) {
    JKRHeap* heap = ensureHeap();
    REQUIRE(heap != nullptr);

    const std::vector<u8> rarc = buildSubdirLayoutArcRarc();

    u8* blob = static_cast<u8*>(JKRHeap::alloc(static_cast<u32>(rarc.size()), 32, heap));
    REQUIRE(blob != nullptr);
    std::memcpy(blob, rarc.data(), rarc.size());

    JKRMemArchive archive;
    REQUIRE(archive.mountFixed(blob, JKR_MEM_BREAK_FLAG_1));
    CHECK(archive.mIsMounted);

    LayoutHolder holder(archive);

    // Every file sits in a subfolder: the enumeration must walk the FULL
    // flat table (14 entries) and fetch by index, not by "/<basename>".
    REQUIRE(holder.mLayoutRes.mCount == 1u);
    CHECK(holder.mLayoutRes.getRes("WiiRemoteStrap") != nullptr);
    CHECK(std::strcmp(holder.mLayoutRes.getResName(0u), "wiiremotestrap") == 0);

    REQUIRE(holder.mAnimRes.mCount == 1u);
    CHECK(holder.mAnimRes.getRes("Strap") != nullptr);

    REQUIRE(holder.mResOther.mCount == 1u);
    CHECK(holder.getResOther("strap_01") != nullptr);

    void* pTpl = holder.GetResource('timg', "strap_01.tpl", nullptr);
    REQUIRE(pTpl != nullptr);
    CHECK(std::memcmp(pTpl, kTplBytes.data(), kTplBytes.size()) == 0);

    void* pBrlan = holder.mAnimRes.getRes("Strap");
    REQUIRE(pBrlan != nullptr);
    CHECK(std::memcmp(pBrlan, kBrlanBytes.data(), kBrlanBytes.size()) == 0);
}

TEST_CASE(layout_anm_player_isstop_without_transform) {
    // Until brlan parsing lands (M9.5.3b) getAnimTransform returns null;
    // the player must report "stopped" so nerves waiting on isAnimStopped
    // keep flowing, and movement/reflectFrame must not dereference null.
    LayoutAnmPlayer player(nullptr);

    CHECK(player.isStop());
    CHECK(player.mAnimTransform == nullptr);

    player.movement();
    player.reflectFrame();
    player.stop();

    CHECK(player.isStop());
}

TEST_CASE(j3d_frame_ctrl_stop_and_loop) {
    // Non-looping (EMode_NONE, what LayoutAnmPlayer::start picks for
    // non-loop brlans): the stop state bit is set at the end frame and the
    // frame clamps just below it.
    J3DFrameCtrl ctrl(4);
    ctrl.init(4);
    ctrl.setAttribute(J3DFrameCtrl::EMode_NONE);
    CHECK(!ctrl.checkState(1));

    for (int i = 0; i < 3; i++) {
        ctrl.update();
    }
    CHECK(!ctrl.checkState(1));  // frame 3 < end 4

    ctrl.update();               // frame 4 >= end -> clamp + stop
    CHECK(ctrl.checkState(1));
    CHECK_NEAR(ctrl.getFrame(), 3.999f, 1e-4f);
    CHECK_NEAR(ctrl.getRate(), 0.0f, 1e-6f);

    // Looping (EMode_LOOP, init default and what start picks for loop
    // brlans): never flags stop; the frame wraps around.
    J3DFrameCtrl loop(4);
    loop.init(4);
    loop.setAttribute(J3DFrameCtrl::EMode_LOOP);
    loop.setLoop(0);

    for (int i = 0; i < 10; i++) {
        loop.update();
        CHECK(!loop.checkState(1));
        CHECK(loop.getFrame() >= 0.0f);
        CHECK(loop.getFrame() < 4.0f);
    }
}

}  // namespace
