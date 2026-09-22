#pragma once
// =============================================================================
// compat/game — GameTextTable (PC_PORT M10.1): the localized strings the
// ported screens need, in the language chosen with --language /
// GALAXY_LANGUAGE (compat/game/LanguageCompat).
//
// -----------------------------------------------------------------------------
// Why this exists (the bug it fixes)
// -----------------------------------------------------------------------------
// On the console every UI string comes from /MessageData/Message.arc (a BMG
// binary + the MessageId.tbl BCSV that maps "Layout_FileSelectTxtStart" to a
// message index). The port reads that same data from the user's disc assets
// (compat/game/GameMessageData — the host BMG/BCSV reader), so with assets
// mounted the strings below are NEVER used: they are only the no-assets
// fallback (headless test runs, a missing dump) that keeps the screens from
// showing the brlyt's authored language — the mix of languages reported in
// M10.1 ("Copy / Icon / Erase" next to a Spanish button).
//
// The layout naming rule the console uses (LayoutCoreUtil::initTextBoxPane) is
//     message id = "Layout_" + layoutName + paneNameWithoutLanguageSuffix
// and the screen-level strings (file-select icons, date/time formats) use
// their own ids ("System_FileSelect_Icon000", "System_Date000", …). This table
// answers those ids for the twelve disc languages, so the same call site works
// once the real BMG reader lands: only the lookup changes, not the screens.
//
// Scope: the strings the ported screens actually render today (the file-select
// screen and the title prompt). Everything else still degrades to the text
// baked in the brlyt, exactly as before.
// =============================================================================

#include <revolution/types.h>

#include <cstddef>

namespace compat::game {

/// Strings the ported screens ask for by id: the game's message data first
/// (GameMessageData, assets), the embedded table when the assets are absent.
const wchar_t* gameTextForMessageId(const char* pMessageId);

/// The embedded no-assets table alone (diagnostics: tells the layout log
/// whether a string came from the disc or from the fallback).
const wchar_t* gameTextFromEmbeddedTable(const char* pMessageId);

/// Convenience wrapper for the one-off literals a host screen draws itself
/// (e.g. the debug overlay's labels). Returns `pFallback` when unknown.
const wchar_t* gameTextOr(const char* pMessageId, const wchar_t* pFallback);

/// Builds "Layout_<layout><pane>" with the language suffix of `pPaneName`
/// stripped ("TxtStartUsEn" -> "Layout_FileSelectTxtStart") into `pOut`.
/// Returns pOut. Helper for the layout text-box initialisation.
char* buildLayoutMessageId(char* pOut, size_t outSize, const char* pLayoutName, const char* pPaneName);

/// True when `pMessageId` is one this table knows (diagnostics/tests).
bool hasGameText(const char* pMessageId);

}  // namespace compat::game
