// =============================================================================
// compat/game — GameTextTable implementation (see GameTextTable.h).
// =============================================================================

#include "compat/game/GameTextTable.h"

#include "compat/game/LanguageCompat.h"
#include "platform/Log/Log.h"

#include <cstdio>
#include <cstring>

namespace compat::game {

namespace {

// Language column order = compat/game/LanguageCompat.cpp kLanguages[]:
//   JpJapanese UsEnglish UsSpanish UsFrench EuEnglish EuSpanish EuFrench
//   EuGerman EuItalian EuDutch CnSimpChinese KrKorean
constexpr int kLangNum = 12;
constexpr int kEn = 1;

struct TextEntry {
    const char* messageId;
    const wchar_t* text[kLangNum];
};

// ---------------------------------------------------------------------------
// The table.
//
// Filled in for the languages whose official wording is verifiable from the
// game's own captures (the French and Japanese columns come straight from a
// retail FileSelect capture: "Jouer avec ces données", "コピー", "アイコン",
// "けす"); the rest are the standard Nintendo translations of the same UI
// labels. A column that is `nullptr` falls back to English, so an unknown
// string never renders as garbage — it renders as the English one.
// ---------------------------------------------------------------------------
const TextEntry sTable[] = {
    // --- the Start button label, "Play This File" --------------------------
    // The pane is FileSelect > StartButton > StartButtonTxt > TxtStart|TxtStart*,
    // so its message id is "Layout_FileSelectTxtStart" — the button label, NOT
    // the "choose a file" line. Two independent confirmations:
    //   * a retail EU capture of this very screen shows "Jouer avec ces
    //     données" on that button (the French "Play This File"), which is what
    //     the arc's plain TxtStart pane carries before the message system
    //     replaces it — the M10.1 build rendered exactly that string;
    //   * the "Please choose a file." bar is the StarPointer's 1P guidance
    //     (StarPointerUtil.cpp:912 request1PGuidance("System_FileSelect008")),
    //     i.e. no layout pane of FileSelect.arc holds it at all: see
    //     compat/ui/GuidanceBanner.h. Assigning this text to TxtStart (the M10.1
    //     table did) would have put "Please choose a file." INSIDE the start
    //     button.
    {"Layout_FileSelectTxtStart",
     {L"このデータで遊ぶ", L"Play This File", L"Juega con estos datos", L"Jouer avec ces données", L"Play This File",
      L"Juega con estos datos", L"Jouer avec ces données", L"Mit diesen Daten spielen", L"Gioca con questi dati",
      L"Met dit bestand spelen", L"使用这个存档", L"이 데이터로 플레이"}},
    {"Layout_FileSelectShaStart",
     {L"このデータで遊ぶ", L"Play This File", L"Juega con estos datos", L"Jouer avec ces données", L"Play This File",
      L"Juega con estos datos", L"Jouer avec ces données", L"Mit diesen Daten spielen", L"Gioca con questi dati",
      L"Met dit bestand spelen", L"使用这个存档", L"이 데이터로 플레이"}},
    {"Layout_FileSelectStartButtonTxt",
     {L"このデータで遊ぶ", L"Play This File", L"Juega con estos datos", L"Jouer avec ces données", L"Play This File",
      L"Juega con estos datos", L"Jouer avec ces données", L"Mit diesen Daten spielen", L"Gioca con questi dati",
      L"Met dit bestand spelen", L"使用这个存档", L"이 데이터로 플레이"}},

    // --- the StarPointer 1P guidance (the "choose a file" bar) --------------
    // Drawn by the host itself (compat/ui/GuidanceBanner) because the string
    // reaches no layout pane: the console puts it in the guidance window when
    // the file-select stage comes up.
    {"System_FileSelect008",
     {L"ファイルをえらんでください", L"Please choose a file.", L"Elige un archivo.", L"Choisis un fichier.",
      L"Please choose a file.", L"Elige un archivo.", L"Choisis un fichier.", L"Bitte wähle eine Datei.",
      L"Scegli un file.", L"Kies een bestand.", L"请选择存档", L"파일을 선택해 주세요"}},

    // --- Copy / Icon / Erase ----------------------------------------------
    {"Layout_FileSelectTxtCopy",
     {L"コピー", L"Copy", L"Copiar", L"Copier", L"Copy", L"Copiar", L"Copier", L"Kopieren", L"Copia", L"Kopiëren", L"复制",
      L"복사"}},
    {"Layout_FileSelectShaCopy",
     {L"コピー", L"Copy", L"Copiar", L"Copier", L"Copy", L"Copiar", L"Copier", L"Kopieren", L"Copia", L"Kopiëren", L"复制",
      L"복사"}},
    {"Layout_FileSelectTxtMii",
     {L"アイコン", L"Icon", L"Icono", L"Icône", L"Icon", L"Icono", L"Icône", L"Symbol", L"Icona", L"Pictogram", L"头像",
      L"아이콘"}},
    {"Layout_FileSelectShaMii",
     {L"アイコン", L"Icon", L"Icono", L"Icône", L"Icon", L"Icono", L"Icône", L"Symbol", L"Icona", L"Pictogram", L"头像",
      L"아이콘"}},
    {"Layout_FileSelectTxtDelete",
     {L"けす", L"Erase", L"Borrar", L"Effacer", L"Erase", L"Borrar", L"Effacer", L"Löschen", L"Cancella", L"Wissen",
      L"删除", L"삭제"}},
    {"Layout_FileSelectShaDelete",
     {L"けす", L"Erase", L"Borrar", L"Effacer", L"Erase", L"Borrar", L"Effacer", L"Löschen", L"Cancella", L"Wissen",
      L"删除", L"삭제"}},

    // --- the Back button (BackButton.arc) ---------------------------------
    {"Layout_BackButtonBack",
     {L"もどる", L"Back", L"Atrás", L"Retour", L"Back", L"Atrás", L"Retour", L"Zurück", L"Indietro", L"Terug", L"返回",
      L"뒤로"}},
    {"Layout_BackButtonTxtBack",
     {L"もどる", L"Back", L"Atrás", L"Retour", L"Back", L"Atrás", L"Retour", L"Zurück", L"Indietro", L"Terug", L"返回",
      L"뒤로"}},

    // --- the P2 badge ------------------------------------------------------
    {"Layout_FileSelect2P", {L"2P", L"2P", L"2J", L"2J", L"2P", L"2J", L"2J", L"2S", L"2G", L"2P", L"2P", L"2P"}},

    // --- the file-select info bar (FileInfo.arc) ---------------------------
    // The name of a file that has no icon yet (a "new" slot).
    {"System_FileSelect_Icon000", {L"マリオ", L"Mario", L"Mario", L"Mario", L"Mario", L"Mario", L"Mario", L"Mario",
                                   L"Mario", L"Mario", L"Mario", L"Mario"}},
    {"System_FileSelect_Icon001", {L"ルイージ", L"Luigi", L"Luigi", L"Luigi", L"Luigi", L"Luigi", L"Luigi", L"Luigi",
                                   L"Luigi", L"Luigi", L"Luigi", L"Luigi"}},
    // "New file" / the empty-slot label.
    {"System_FileSelect_NewFile", {L"あたらしいファイル", L"New File", L"Archivo nuevo", L"Nouveau fichier", L"New File",
                                   L"Archivo nuevo", L"Nouveau fichier", L"Neue Datei", L"Nuovo file", L"Nieuw bestand",
                                   L"新存档", L"새 파일"}},
    // Date / time formats (System_Date000 = yy/mm/dd, System_Time002 = hh:mm).
    {"System_Date000", {L"%04d/%02d/%02d", L"%02d/%02d/%04d", L"%02d/%02d/%04d", L"%02d/%02d/%04d", L"%02d/%02d/%04d",
                        L"%02d/%02d/%04d", L"%02d/%02d/%04d", L"%02d.%02d.%04d", L"%02d/%02d/%04d", L"%02d-%02d-%04d",
                        L"%04d/%02d/%02d", L"%04d.%02d.%02d"}},
    {"System_Time002", {L"%02d:%02d", L"%02d:%02d", L"%02d:%02d", L"%02d:%02d", L"%02d:%02d", L"%02d:%02d", L"%02d:%02d",
                        L"%02d:%02d", L"%02d:%02d", L"%02d:%02d", L"%02d:%02d", L"%02d:%02d"}},
};

// The language-column index of the active language (see kLanguages order).
int activeColumn() {
    const u32 n = compat::getLanguageNum();

    for (u32 i = 0; i < n; ++i) {
        if (std::strcmp(compat::getLanguageNameByIndex(i), compat::getLanguageName()) == 0) {
            return static_cast< int >(i);
        }
    }

    return kEn;
}

/// Strips a trailing language suffix ("TxtStartUsEn" -> "TxtStart"). The four
/// suffix letters are the ones LanguageCompat lists ([A-Z][a-z][A-Z][a-z]).
size_t stripLanguageSuffix(const char* pName, char* pOut, size_t outSize) {
    size_t len = std::strlen(pName);

    if (len > 4) {
        const char* tail = pName + len - 4;
        const bool looksLikeSuffix = tail[0] >= 'A' && tail[0] <= 'Z' && tail[1] >= 'a' && tail[1] <= 'z' &&
                                     tail[2] >= 'A' && tail[2] <= 'Z' && tail[3] >= 'a' && tail[3] <= 'z';

        if (looksLikeSuffix) {
            len -= 4;
        }
    }

    if (len >= outSize) {
        len = outSize - 1;
    }

    std::memcpy(pOut, pName, len);
    pOut[len] = '\0';

    return len;
}

}  // namespace

char* buildLayoutMessageId(char* pOut, size_t outSize, const char* pLayoutName, const char* pPaneName) {
    if (pOut == nullptr || outSize == 0) {
        return pOut;
    }

    char pane[64];
    stripLanguageSuffix(pPaneName != nullptr ? pPaneName : "", pane, sizeof(pane));

    std::snprintf(pOut, outSize, "Layout_%s%s", pLayoutName != nullptr ? pLayoutName : "", pane);

    return pOut;
}

const wchar_t* gameTextForMessageId(const char* pMessageId) {
    if (pMessageId == nullptr || *pMessageId == '\0') {
        return nullptr;
    }

    const int column = activeColumn();

    for (const TextEntry& entry : sTable) {
        if (std::strcmp(entry.messageId, pMessageId) != 0) {
            continue;
        }

        const wchar_t* text = entry.text[column];

        if (text == nullptr) {
            text = entry.text[kEn];
        }

        return text;
    }

    return nullptr;
}

const wchar_t* gameTextOr(const char* pMessageId, const wchar_t* pFallback) {
    const wchar_t* text = gameTextForMessageId(pMessageId);

    return text != nullptr ? text : pFallback;
}

bool hasGameText(const char* pMessageId) {
    return gameTextForMessageId(pMessageId) != nullptr;
}

}  // namespace compat::game
