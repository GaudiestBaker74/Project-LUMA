// =============================================================================
// GameMessageData tests (PC_PORT): the host reader of the game's own message
// data (/MessageData/Message.arc — MessageId.tbl BCSV + Message.bmg INF1/DAT1).
//
// The real dump cannot ship with the repo, so these cases synthesise the two
// binaries byte-for-byte in the disc format (big-endian BCSV + BMG blocks,
// UTF-16BE strings, surrogate pairs included) and feed them through the same
// parseMessageDataBuffers() that initGameMessageData() runs over the mounted
// arc. They pin:
//
//   1. the BCSV field lookup (JGadget hash of "MessageId"/"Index") and the
//      STRING_PTR / LONG column decoding on a little-endian host;
//   2. the INF1 index -> DAT1 string mapping, UTF-16BE -> wide conversion and
//      surrogate-pair merging;
//   3. the precedence: with message data loaded, gameTextForMessageId()
//      answers from the disc, not from the embedded fallback table — "use the
//      game's assets" is the behaviour, not just the intent.
// =============================================================================

#include "compat/game/GameMessageData.h"
#include "compat/game/GameTextTable.h"
#include "compat/game/LanguageCompat.h"

#include "test_runner.h"

#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace {

void putU16BE(std::vector< u8 >& v, u16 x) {
    v.push_back(static_cast< u8 >(x >> 8));
    v.push_back(static_cast< u8 >(x & 0xFF));
}

void putU32BE(std::vector< u8 >& v, u32 x) {
    v.push_back(static_cast< u8 >(x >> 24));
    v.push_back(static_cast< u8 >((x >> 16) & 0xFF));
    v.push_back(static_cast< u8 >((x >> 8) & 0xFF));
    v.push_back(static_cast< u8 >(x & 0xFF));
}

/// JGadget::getHashCode (hash = c + hash * 31) — how BCSV names its columns.
u32 nameHash(const char* pName) {
    u32 hash = 0;
    for (; *pName != '\0'; ++pName) {
        hash = static_cast< u32 >(*pName) + hash * 31u;
    }
    return hash;
}

void putUtf16BE(std::vector< u8 >& v, const std::u16string& s) {
    for (char16_t c : s) {
        putU16BE(v, static_cast< u16 >(c));
    }
    putU16BE(v, 0);
}

// The synthetic disc: three messages. The strings deliberately differ from
// every embedded-table column so a hit can only come from the "disc".
//   index 0 -> "EraseFromDisc"   (Layout_FileSelectTxtDelete)
//   index 1 -> "CopyFromDisc"    (Layout_FileSelectTxtCopy)
//   index 2 -> U+1F31F + 'x'     (System_TestStar, surrogate pair on the wire)
std::vector< u8 > makeBcsv() {
    constexpr u32 kEntrySize = 8; // u32 string-table offset + u32 index
    constexpr u32 kNumEntries = 3;
    constexpr u32 kNumFields = 2;
    const u32 recordsOff = 16 + kNumFields * 12;

    std::vector< u8 > v;
    putU32BE(v, kNumEntries);
    putU32BE(v, kNumFields);
    putU32BE(v, recordsOff);
    putU32BE(v, kEntrySize);

    // Field 0: "MessageId", STRING_PTR (u32 offset into the string table).
    putU32BE(v, nameHash("MessageId"));
    putU32BE(v, 0); // mask
    putU16BE(v, 0); // offsData
    v.push_back(0); // shift
    v.push_back(6); // type STRING_PTR
    // Field 1: "Index", LONG.
    putU32BE(v, nameHash("Index"));
    putU32BE(v, 0xFFFFFFFF);
    putU16BE(v, 4);
    v.push_back(0);
    v.push_back(0); // type LONG

    const char* ids[kNumEntries] = {"Layout_FileSelectTxtDelete", "Layout_FileSelectTxtCopy", "System_TestStar"};
    u32 offsets[kNumEntries] = {0, 0, 0};
    u32 cursor = 0;
    for (u32 i = 0; i < kNumEntries; ++i) {
        offsets[i] = cursor;
        cursor += static_cast< u32 >(std::strlen(ids[i])) + 1;
    }
    // STRING_PTR values are offsets RELATIVE to the string table (the parser
    // adds the table base itself, like JMapInfo::getValueFast).
    const u32 indexes[kNumEntries] = {0, 1, 2};
    for (u32 i = 0; i < kNumEntries; ++i) {
        putU32BE(v, offsets[i]);
        putU32BE(v, indexes[i]);
    }
    for (u32 i = 0; i < kNumEntries; ++i) {
        for (const char* c = ids[i]; *c != '\0'; ++c) {
            v.push_back(static_cast< u8 >(*c));
        }
        v.push_back(0);
    }
    return v;
}

std::vector< u8 > makeBmg() {
    // DAT1 payload first (offsets are relative to it).
    std::vector< u8 > dat;
    const u32 off0 = static_cast< u32 >(dat.size());
    putUtf16BE(dat, u"EraseFromDisc");
    const u32 off1 = static_cast< u32 >(dat.size());
    putUtf16BE(dat, u"CopyFromDisc");
    const u32 off2 = static_cast< u32 >(dat.size());
    putUtf16BE(dat, u"\U0001F31Fx");

    const u32 infSize = 16 + 3 * 4;
    const u32 datSize = 8 + static_cast< u32 >(dat.size());

    std::vector< u8 > v;
    // 0x20-byte BMG file header; the walker only reads the block count at +0xC.
    v.resize(0x20, 0);
    v[0] = 'M';
    v[1] = 'S';
    v[2] = 'G';
    v[3] = 0;
    v[0xC + 3] = 2; // two blocks (u32BE)

    // INF1
    const u8 i0 = 'I', i1 = 'N', i2 = 'F', i3 = '1';
    v.push_back(i0);
    v.push_back(i1);
    v.push_back(i2);
    v.push_back(i3);
    putU32BE(v, infSize);
    putU16BE(v, 3); // itemCount
    putU16BE(v, 4); // itemSize (the parser requires >= 4; items carry a u32)
    putU32BE(v, 0); // _C
    putU32BE(v, off0);
    putU32BE(v, off1);
    putU32BE(v, off2);

    // DAT1
    v.push_back('D');
    v.push_back('A');
    v.push_back('T');
    v.push_back('1');
    putU32BE(v, datSize);
    v.insert(v.end(), dat.begin(), dat.end());
    return v;
}

} // namespace

TEST_CASE(game_message_data_parses_the_disc_format) {
    const std::vector< u8 > tbl = makeBcsv();
    const std::vector< u8 > bmg = makeBmg();

    REQUIRE(compat::game::parseMessageDataBuffers(tbl.data(), bmg.data()));
    CHECK(compat::game::gameMessageDataLoaded());

    const wchar_t* pErase = compat::game::gameMessageFromAssets("Layout_FileSelectTxtDelete");
    REQUIRE(pErase != nullptr);
    CHECK(std::wcscmp(pErase, L"EraseFromDisc") == 0);

    const wchar_t* pCopy = compat::game::gameMessageFromAssets("Layout_FileSelectTxtCopy");
    REQUIRE(pCopy != nullptr);
    CHECK(std::wcscmp(pCopy, L"CopyFromDisc") == 0);

    // Surrogate pair on the wire -> single wide char off the BMP.
    const wchar_t* pStar = compat::game::gameMessageFromAssets("System_TestStar");
    REQUIRE(pStar != nullptr);
    CHECK(pStar[0] == static_cast< wchar_t >(0x1F31F));
    CHECK(pStar[1] == L'x');
    CHECK(pStar[2] == L'\0');

    CHECK(compat::game::gameMessageFromAssets("Layout_FileSelectNoSuchPane") == nullptr);

    compat::game::resetGameMessageDataForTest();
    CHECK(!compat::game::gameMessageDataLoaded());
}

TEST_CASE(game_message_data_wins_over_the_embedded_table) {
    // With the "disc" mounted, the combined accessor must answer from the
    // assets even for ids the fallback table also knows — the screen shows
    // the game's own strings, not the PC-authored ones.
    const std::vector< u8 > tbl = makeBcsv();
    const std::vector< u8 > bmg = makeBmg();
    REQUIRE(compat::game::parseMessageDataBuffers(tbl.data(), bmg.data()));
    REQUIRE(compat::setLanguage("UsEnglish"));

    const wchar_t* pCopy = compat::game::gameTextForMessageId("Layout_FileSelectTxtCopy");
    REQUIRE(pCopy != nullptr);
    CHECK(std::wcscmp(pCopy, L"CopyFromDisc") == 0); // not the table's "Copy"

    // An id the disc does not carry still falls through to the table.
    const wchar_t* pBack = compat::game::gameTextForMessageId("Layout_BackButtonBack");
    REQUIRE(pBack != nullptr);
    CHECK(std::wcscmp(pBack, L"Back") == 0);

    compat::game::resetGameMessageDataForTest();
    compat::setLanguage("UsEnglish");

    // Without message data the table answers again (the no-assets fallback).
    const wchar_t* pTableCopy = compat::game::gameTextForMessageId("Layout_FileSelectTxtCopy");
    REQUIRE(pTableCopy != nullptr);
    CHECK(std::wcscmp(pTableCopy, L"Copy") == 0);
}
