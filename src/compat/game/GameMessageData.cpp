// =============================================================================
// compat/game — GameMessageData implementation (see GameMessageData.h).
//
// Mirrors petari's Game/System/MessageHolder.cpp (MessageData::MessageData +
// findMessageIndex) but reads the big-endian Wii structures on a
// little-endian host, and without dragging the TalkNode/TalkMessageInfo graph
// into the build: all the layout/text consumers need is "message id -> the
// UTF-16 string the disc ships".
// =============================================================================

#include "compat/game/GameMessageData.h"

#include "compat/game/LanguageCompat.h"
#include "platform/Log/Log.h"

#include "Game/Util/FileUtil.hpp"
#include <JSystem/JKernel/JKRMemArchive.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

namespace compat::game {

namespace {

// --- big-endian readers (the disc data is BE; the host need not be) --------
u16 readU16BE(const u8* p) {
    return static_cast< u16 >((static_cast< u16 >(p[0]) << 8) | p[1]);
}

u32 readU32BE(const u8* p) {
    return (static_cast< u32 >(p[0]) << 24) | (static_cast< u32 >(p[1]) << 16) | (static_cast< u32 >(p[2]) << 8) |
           static_cast< u32 >(p[3]);
}

/// JGadget::getHashCode (petari src/JSystem/JGadget/hashcode.cpp):
/// hash = c + hash * 31 over the NUL-terminated name. BCSV fields carry this
/// hash instead of the name, so it is how "MessageId"/"Index" are recognised.
u32 hashCode(const char* pName) {
    u32 hash = 0;
    for (; *pName != '\0'; ++pName) {
        hash = static_cast< u32 >(*pName) + hash * 31u;
    }
    return hash;
}

// BCSV column storage types the message table can carry (JMapInfo values).
constexpr u32 kTypeLong = 0;
constexpr u32 kTypeString = 1;
constexpr u32 kTypeShort = 4;
constexpr u32 kTypeByte = 5;
constexpr u32 kTypeStringPtr = 6;

struct BcsvField {
    u32 hash;
    u32 mask;
    u16 offsData;
    u8 shift;
    u8 type;
};

bool sAttempted = false;
bool sLoaded = false;
std::unordered_map< std::string, std::wstring >* sMessages = nullptr;

/// UTF-16BE (as stored in DAT1) -> host wide string, surrogate pairs merged.
std::wstring utf16BEToWide(const u8* p) {
    std::wstring out;
    for (size_t i = 0;; i += 2) {
        const u16 unit = readU16BE(p + i);
        if (unit == 0) {
            break;
        }
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            const u16 low = readU16BE(p + i + 2);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                out += static_cast< wchar_t >(0x10000u + ((static_cast< u32 >(unit) - 0xD800u) << 10) +
                                              (low - 0xDC00u));
                i += 2;
                continue;
            }
        }
        out += static_cast< wchar_t >(unit);
    }
    return out;
}

/// The BMG block walker of MessageHolder.cpp's getBlock(), BE-safe. Blocks
/// start at 0x20 (after the 0x20-byte file header whose +0xC word is the
/// block count).
const u8* findBmgBlock(const u8* pBmg, u32 magic) {
    const u32 numBlocks = readU32BE(pBmg + 0xC);
    const u8* p = pBmg + 0x20;
    for (u32 i = 0; i < numBlocks; ++i) {
        const u32 blockMagic = readU32BE(p);
        const u32 blockSize = readU32BE(p + 4);
        if (blockMagic == magic) {
            return p;
        }
        if (blockSize == 0) {
            break;
        }
        p += blockSize;
    }
    return nullptr;
}

constexpr u32 kMagicINF1 = 0x494E4631; // 'INF1'
constexpr u32 kMagicDAT1 = 0x44415431; // 'DAT1'

} // namespace

/// Parses MessageId.tbl (BCSV) + Message.bmg (INF1/DAT1) into sMessages.
bool parseMessageDataBuffers(const u8* pTbl, const u8* pBmg) {
    // --- MessageId.tbl: BCSV header ----------------------------------------
    const s32 numEntries = static_cast< s32 >(readU32BE(pTbl + 0));
    const s32 numFields = static_cast< s32 >(readU32BE(pTbl + 4));
    const s32 dataOffset = static_cast< s32 >(readU32BE(pTbl + 8));
    const u32 entrySize = readU32BE(pTbl + 12);
    if (numEntries <= 0 || numFields <= 0 || entrySize == 0) {
        return false;
    }

    BcsvField idField;
    BcsvField indexField;
    bool haveId = false;
    bool haveIndex = false;
    const u32 idHash = hashCode("MessageId");
    const u32 indexHash = hashCode("Index");
    for (s32 f = 0; f < numFields; ++f) {
        const u8* pF = pTbl + 16 + f * 12;
        BcsvField field;
        field.hash = readU32BE(pF + 0);
        field.mask = readU32BE(pF + 4);
        field.offsData = readU16BE(pF + 8);
        field.shift = static_cast< u8 >(pF[10]);
        field.type = static_cast< u8 >(pF[11]);
        if (field.hash == idHash) {
            idField = field;
            haveId = true;
        }
        if (field.hash == indexHash) {
            indexField = field;
            haveIndex = true;
        }
    }
    if (!haveId || !haveIndex) {
        return false;
    }

    const u8* pRecords = pTbl + dataOffset;
    const char* pStringTable = reinterpret_cast< const char* >(pRecords) + static_cast< size_t >(numEntries) * entrySize;

    // --- Message.bmg: INF1 (index -> string offset) + DAT1 (the strings) ----
    const u8* pInf = findBmgBlock(pBmg, kMagicINF1);
    const u8* pDat = findBmgBlock(pBmg, kMagicDAT1);
    if (pInf == nullptr || pDat == nullptr) {
        return false;
    }
    const u32 itemCount = readU16BE(pInf + 8);
    const u32 itemSize = readU16BE(pInf + 10);
    const u8* pItems = pInf + 16;
    const u8* pStrings = pDat + 8;
    if (itemSize < 4) {
        return false;
    }

    auto* pOut = new std::unordered_map< std::string, std::wstring >();
    s32 added = 0;

    for (s32 e = 0; e < numEntries; ++e) {
        const u8* pRec = pRecords + static_cast< size_t >(e) * entrySize;

        // The message id (a string column).
        const char* pId = nullptr;
        if (idField.type == kTypeStringPtr) {
            pId = pStringTable + readU32BE(pRec + idField.offsData);
        } else if (idField.type == kTypeString) {
            pId = reinterpret_cast< const char* >(pRec + idField.offsData);
        } else {
            continue;
        }

        // The message index (an integer column).
        u32 raw = 0;
        switch (indexField.type) {
        case kTypeLong:
            raw = readU32BE(pRec + indexField.offsData);
            break;
        case kTypeShort:
            raw = readU16BE(pRec + indexField.offsData);
            break;
        case kTypeByte:
            raw = pRec[indexField.offsData];
            break;
        default:
            continue;
        }
        const u32 index = (raw & indexField.mask) >> indexField.shift;

        if (index >= itemCount) {
            continue;
        }
        const u32 stringOffset = readU32BE(pItems + index * itemSize);
        pOut->emplace(pId, utf16BEToWide(pStrings + stringOffset));
        ++added;
    }

    if (added == 0) {
        delete pOut;
        return false;
    }

    sMessages = pOut;
    sLoaded = true;
    sAttempted = true; // the data is authoritative; do not re-init from assets
    PL_LOG_INFO("compat.msg", "message data parsed: %d message(s) from the game's Message.bmg", added);
    return true;
}

namespace {

void initImpl() {
    sAttempted = true;

    // Language-first, like the game's makeFileNameConsideringLanguage: the
    // retail disc carries the localized copy under /<Language>/ and some
    // dumps a single tree at the root.
    char localizedPath[256];
    std::snprintf(localizedPath, sizeof(localizedPath), "/%s/MessageData/Message.arc", compat::getLanguageName());

    JKRMemArchive* pArchive = MR::mountArchive(localizedPath, nullptr);
    const char* pUsedPath = localizedPath;
    if (pArchive == nullptr) {
        pArchive = MR::mountArchive("/MessageData/Message.arc", nullptr);
        pUsedPath = "/MessageData/Message.arc";
    }
    if (pArchive == nullptr) {
        PL_LOG_WARN("compat.msg",
                    "Message.arc not in the assets tree (tried '%s' and '/MessageData/Message.arc') — "
                    "UI text falls back to the embedded table",
                    localizedPath);
        return;
    }

    void* pBmg = pArchive->getResource("/Message.bmg");
    void* pTbl = pArchive->getResource("/MessageId.tbl");
    if (pBmg == nullptr) {
        pBmg = pArchive->getResource("Message.bmg");
    }
    if (pTbl == nullptr) {
        pTbl = pArchive->getResource("MessageId.tbl");
    }
    if (pBmg == nullptr || pTbl == nullptr) {
        PL_LOG_WARN("compat.msg", "'%s' misses %s — UI text falls back to the embedded table", pUsedPath,
                    pBmg == nullptr ? "Message.bmg" : "MessageId.tbl");
        return;
    }

    if (!parseMessageDataBuffers(static_cast< const u8* >(pTbl), static_cast< const u8* >(pBmg))) {
        PL_LOG_WARN("compat.msg", "'%s' does not parse as MessageId.tbl + Message.bmg — embedded fallback stays",
                    pUsedPath);
        return;
    }

    sLoaded = true;
    PL_LOG_INFO("compat.msg", "game message data mounted from '%s' (%s)", pUsedPath, compat::getLanguageName());
}

} // namespace

void initGameMessageData() {
    if (!sAttempted) {
        initImpl();
    }
}

bool gameMessageDataLoaded() {
    return sLoaded;
}

const wchar_t* gameMessageFromAssets(const char* pMessageId) {
    if (!sAttempted) {
        initImpl();
    }
    if (!sLoaded || pMessageId == nullptr || sMessages == nullptr) {
        return nullptr;
    }
    const auto it = sMessages->find(pMessageId);
    return it != sMessages->end() ? it->second.c_str() : nullptr;
}

void resetGameMessageDataForTest() {
    delete sMessages;
    sMessages = nullptr;
    sLoaded = false;
    sAttempted = false;
}

} // namespace compat::game
