#pragma once
// =============================================================================
// compat/game — host replacement of Game/System/Language.cpp (M9.5.4 v6).
//
// The console derives the game language from the IPL setting (SCGetLanguage)
// and the disc region (Language.cpp: cSCLanguage2GameLanguageTable). The host
// has neither, so the language is a runtime choice:
//   1. `--language <folder>` on the command line (compat::setLanguage), else
//   2. the GALAXY_LANGUAGE environment variable, else
//   3. "UsEnglish".
// Valid names are the language folders of the disc (Language.cpp
// cLanguages[]): JpJapanese UsEnglish UsSpanish UsFrench EuEnglish EuSpanish
// EuFrench EuGerman EuItalian EuDutch CnSimpChinese KrKorean.
//
// Consumers:
//   * MR::getCurrentLanguagePrefix / MR::getCurrentRegionPrefix (the vendored
//     ISBN/censorship branch of LogoScene checks the region for "Cn").
//   * LayoutManager::removeUnnecessaryPanes — SMG layouts carry language
//     variants of a pane as siblings whose name ends with the first four
//     letters of the language folder ("TxtStart", "TxtStartJpJa",
//     "TxtStartUsEn"); only the variant of the current language survives.
// =============================================================================
#include <revolution/types.h>

namespace compat {

// Selects the language by folder name (case-insensitive). Returns false and
// keeps the current selection when the name is not one of the twelve.
bool setLanguage(const char* pFolderName);

// Folder name of the current language ("UsEnglish", ...).
const char* getLanguageName();

// Region part of the current language ("Jp", "Us", "Eu", "Cn" or "Kr").
const char* getLanguageRegion();

// The 4-letter pane-name suffix of the current language ("UsEn", ...).
const char* getLanguagePaneSuffix();

// Number of known languages / folder name by index (--help, tests).
u32 getLanguageNum();
const char* getLanguageNameByIndex(u32 index);

} // namespace compat
