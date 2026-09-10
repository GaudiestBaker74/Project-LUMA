// =============================================================================
// compat/game — host language selection (see LanguageCompat.h).
// =============================================================================
#include "compat/game/LanguageCompat.h"

#include "platform/Log/Log.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace {

struct HostLanguage {
    const char* name;    // disc folder / MR::getCurrentLanguagePrefix()
    const char* region;  // MR::getCurrentRegionPrefix()
    const char* suffix;  // pane-name suffix (first 4 letters of the folder)
};

// Same order as petari's Language.cpp cLanguages[] (Game/System/Language.cpp).
const HostLanguage kLanguages[] = {
    {"JpJapanese", "Jp", "JpJa"},   {"UsEnglish", "Us", "UsEn"}, {"UsSpanish", "Us", "UsSp"},
    {"UsFrench", "Us", "UsFr"},     {"EuEnglish", "Eu", "EuEn"}, {"EuSpanish", "Eu", "EuSp"},
    {"EuFrench", "Eu", "EuFr"},     {"EuGerman", "Eu", "EuGe"},  {"EuItalian", "Eu", "EuIt"},
    {"EuDutch", "Eu", "EuDu"},      {"CnSimpChinese", "Cn", "CnSi"}, {"KrKorean", "Kr", "KrKo"},
};
const u32 kLanguageNum = static_cast< u32 >(sizeof(kLanguages) / sizeof(kLanguages[0]));
const u32 kDefaultLanguage = 1;  // UsEnglish

u32 sCurrent = kDefaultLanguage;
bool sInitialized = false;

bool equalsIgnoreCase(const char* a, const char* b) {
    while (*a != '\0' && *b != '\0') {
        if (std::tolower(static_cast< unsigned char >(*a)) != std::tolower(static_cast< unsigned char >(*b))) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

s32 findLanguage(const char* pName) {
    if (pName == nullptr || *pName == '\0') {
        return -1;
    }
    for (u32 i = 0; i < kLanguageNum; ++i) {
        if (equalsIgnoreCase(kLanguages[i].name, pName) || equalsIgnoreCase(kLanguages[i].suffix, pName)) {
            return static_cast< s32 >(i);
        }
    }
    return -1;
}

// Lazy default: the environment is consulted the first time the language is
// read unless setLanguage() ran before (command line wins over environment).
void ensureInitialized() {
    if (sInitialized) {
        return;
    }
    sInitialized = true;
    if (const char* env = std::getenv("GALAXY_LANGUAGE"); env != nullptr && *env != '\0') {
        const s32 idx = findLanguage(env);
        if (idx >= 0) {
            sCurrent = static_cast< u32 >(idx);
        } else {
            PL_LOG_WARN("compat.lang", "GALAXY_LANGUAGE='%s' is not a known language folder; using %s", env,
                        kLanguages[sCurrent].name);
        }
    }
    PL_LOG_INFO("compat.lang", "game language: %s (region %s, pane suffix %s)", kLanguages[sCurrent].name,
                kLanguages[sCurrent].region, kLanguages[sCurrent].suffix);
}

} // namespace

namespace compat {

bool setLanguage(const char* pFolderName) {
    const s32 idx = findLanguage(pFolderName);
    if (idx < 0) {
        return false;
    }
    sCurrent = static_cast< u32 >(idx);
    sInitialized = true;
    PL_LOG_INFO("compat.lang", "game language: %s (region %s, pane suffix %s)", kLanguages[sCurrent].name,
                kLanguages[sCurrent].region, kLanguages[sCurrent].suffix);
    return true;
}

const char* getLanguageName() {
    ensureInitialized();
    return kLanguages[sCurrent].name;
}

const char* getLanguageRegion() {
    ensureInitialized();
    return kLanguages[sCurrent].region;
}

const char* getLanguagePaneSuffix() {
    ensureInitialized();
    return kLanguages[sCurrent].suffix;
}

u32 getLanguageNum() {
    return kLanguageNum;
}

const char* getLanguageNameByIndex(u32 index) {
    return index < kLanguageNum ? kLanguages[index].name : "";
}

} // namespace compat
