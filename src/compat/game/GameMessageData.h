#pragma once
// =============================================================================
// compat/game — the game's own message strings, read from the disc assets.
//
// On the console every UI string comes from /MessageData/Message.arc (a BMG
// binary + the MessageId.tbl BCSV that maps "Layout_FileSelectTxtStart" to a
// message index — see petari's Game/System/MessageHolder.cpp). This module is
// the host-side reader of that same data, so the port renders the strings the
// game ships instead of anything authored on the PC side:
//
//   * the arc is mounted language-first, exactly like the game's
//     makeFileNameConsideringLanguage ("/<Language>/MessageData/Message.arc",
//     falling back to "/MessageData/Message.arc" when the disc carries a
//     single copy);
//   * MessageId.tbl is parsed as the big-endian BCSV it is (field lookup by
//     the JGadget hash of "MessageId"/"Index", like JMapInfo::searchItemInfo);
//   * Message.bmg INF1/DAT1 blocks give the UTF-16BE string per index.
//
// Everything is BE-safe on the host (the Wii data is big-endian, the host is
// not). When the assets are absent (headless test runs) the module reports
// "not loaded" and callers keep their fallbacks.
// =============================================================================

#include <revolution/types.h>

namespace compat::game {

/// Mounts + parses the message archive once (idempotent). Safe to call from
/// any thread that owns the frame loop; does nothing after the first call.
void initGameMessageData();

/// True once the archive was mounted and parsed (initGameMessageData ran and
/// found usable Message.bmg + MessageId.tbl in the assets).
bool gameMessageDataLoaded();

/// The UTF-16 message the disc carries for pMessageId (e.g.
/// "Layout_FileSelectTxtStart"), converted to a host wide string, or nullptr
/// when the id is not in the table or the data is not loaded. The returned
/// pointer stays valid for the process lifetime.
const wchar_t* gameMessageFromAssets(const char* pMessageId);

/// Parses in-memory big-endian MessageId.tbl (BCSV) + Message.bmg buffers and
/// publishes them as the message source — the same code initGameMessageData
/// runs over the mounted arc, factored out so the unit test feeds synthetic
/// disc-format buffers straight in. False leaves the previous state untouched.
bool parseMessageDataBuffers(const u8* pTbl, const u8* pBmg);

/// Test hook: drops whatever the parsers published and marks the archive as
/// never attempted, so the next query re-initialises from the assets.
void resetGameMessageDataForTest();

} // namespace compat::game
