// =============================================================================
// PC_PORT PATCH of the vendored Game/System/LayoutHolder.cpp (M9.5.3a).
//
// Upstream state: the constructor/destructor are decompiled, but
// `initializeArc` is commented out and `initEachResTable`, `GetResource`,
// `GetFont`, `mount` and the getResOther* accessors have no bodies at all.
//
// This file keeps the recovered parts verbatim and reconstructs the rest:
//   * initializeArc scans the mounted RARC with the M9.5.1 archive API
//     (countResource/getDirEntry/getResource) and classifies every file into
//     the three ResTables, exactly like the console original:
//       - "*.brlyt"            -> mLayoutRes
//       - "*.brlan"            -> mAnimRes
//       - everything else      -> mResOther   (tpl textures, brfnt fonts, ...)
//     Names are stored with the extension stripped (ResFileInfo::setName
//     stripExt=true) because nw4r lyt asks for resources by their in-brlyt
//     names, and those may or may not carry an extension.
//   * GetResource('timg'/'font', name) serves blobs from mResOther. The query
//     name is tried raw first, then with its extension stripped, so both
//     "foo.tpl" and "foo" hit the same entry. TPL/brfnt big-endian conversion
//     happens lazily inside the M9.5.2 hosts (TexMap::ReplaceImage /
//     ResFont::SetResource), so blobs are served raw here.
//   * GetFont returns null for now: SMG fonts live in the global
//     /LayoutData/Font.arc (GameSystemFontHolder), whose host wiring lands
//     with text drawing in M9.5.3c. TextBox falls back to
//     GetResource('font', name) for arcs that carry their own brfnt.
//   * mount(char*) was only ever seen called as mount(nullptr) from the
//     commented-out initializeArc; reconstructed as a documented no-op.
// =============================================================================
#include "Game/System/LayoutHolder.hpp"
#include <JSystem/JKernel/JKRArchive.hpp>
#include <cstring>
#include <cstdio>

#include "platform/Log/Log.h"

namespace {
    const char* sLayoutExt[] = {
        ".brlyt",
        nullptr,
    };
    const char* sAnimationExt[] = {
        ".brlan",
        nullptr,
    };

    bool endsWithAnyExt(const char* pName, const char* const* pExts) {
        size_t nameLen = strlen(pName);

        for (const char* const* ext = pExts; *ext != nullptr; ext++) {
            size_t extLen = strlen(*ext);

            if (nameLen < extLen) {
                continue;
            }

            // Case-insensitive compare without strcasecmp (MSVC portability).
            const char* pTail = pName + nameLen - extLen;
            const char* pExt = *ext;
            bool match = true;

            for (size_t i = 0; i < extLen; i++) {
                char a = pTail[i];
                char b = pExt[i];

                if (a >= 'A' && a <= 'Z') {
                    a = static_cast< char >(a - 'A' + 'a');
                }

                if (b >= 'A' && b <= 'Z') {
                    b = static_cast< char >(b - 'A' + 'a');
                }

                if (a != b) {
                    match = false;
                    break;
                }
            }

            if (match) {
                return true;
            }
        }

        return false;
    }

    // Strips the last ".ext" (if any) into the caller buffer and returns it.
    const char* stripExtInto(const char* pName, char* pOut, size_t outSize) {
        snprintf(pOut, outSize, "%s", pName);
        char* dot = strrchr(pOut, '.');

        if (dot != nullptr) {
            dot[0] = '\0';
        }

        return pOut;
    }
};  // namespace

LayoutHolder::LayoutHolder(JKRArchive& rArchive) : nw4r::lyt::ResourceAccessor(), mArchive(&rArchive) {
    initializeArc();
}

LayoutHolder::~LayoutHolder() {
}

u32 LayoutHolder::initEachResTable(ResTable* pTable, const char* const* pExts) {
    // First pass: count matching entries so the table can be sized.
    //
    // NOTE: getDirEntry() indexes the archive's FULL flat entry table (which
    // also contains directory entries), while countResource() only counts the
    // file entries. Iterating i < countResource() therefore truncates the tail
    // of the table: real SMG layout arcs store their files in subfolders
    // (anm/, blyt/, timg/), so whole folders at the end were never seen.
    // Scan until getDirEntry() reports the end of the table instead.
    u32 count = 0;

    for (u32 i = 0;; i++) {
        JKRArchive::SDirEntry dir;

        if (!mArchive->getDirEntry(&dir, i)) {
            break;
        }

        if (((dir.mFileFlag >> JKRArchive::FILE_FLAG_FOLDER_SHIFT) & 1) != 0) {
            continue;
        }

        if (endsWithAnyExt(dir.mName, pExts)) {
            count++;
        }
    }

    if (count == 0) {
        return 0;
    }

    // Second pass: load each resource fully into the current heap and
    // register it under its base name (stripped of the extension).
    pTable->newFileInfoTable(count);

    for (u32 i = 0;; i++) {
        JKRArchive::SDirEntry dir;

        if (!mArchive->getDirEntry(&dir, i)) {
            break;
        }

        if (((dir.mFileFlag >> JKRArchive::FILE_FLAG_FOLDER_SHIFT) & 1) != 0) {
            continue;
        }

        if (!endsWithAnyExt(dir.mName, pExts)) {
            continue;
        }

        // Fetch by flat-table index: entries living in subfolders (blyt/,
        // anm/, timg/) are not reachable through a "/<basename>" path lookup,
        // which only walks the root folder.
        void* pRes = mArchive->getIdxResource(i);

        if (pRes == nullptr) {
            PL_LOG_ERROR("compat.layout", "LayoutHolder: failed to load '%s' from the layout arc", dir.mName);
            continue;
        }

        pTable->add(dir.mName, pRes, true);
    }

    return pTable->mCount;
}

void LayoutHolder::initializeArc() {
    // Count every file entry in the FULL flat table (see initEachResTable for
    // why countResource() must not be used as an iteration bound).
    u32 resCount = 0;

    for (u32 i = 0;; i++) {
        JKRArchive::SDirEntry dir;

        if (!mArchive->getDirEntry(&dir, i)) {
            break;
        }

        if (((dir.mFileFlag >> JKRArchive::FILE_FLAG_FOLDER_SHIFT) & 1) == 0) {
            resCount++;
        }
    }

    resCount -= initEachResTable(&mLayoutRes, sLayoutExt);
    resCount -= initEachResTable(&mAnimRes, sAnimationExt);

    if (resCount > 0) {
        // Everything else (textures, fonts, ...) lands in mResOther.
        mResOther.newFileInfoTable(resCount);

        for (u32 i = 0;; i++) {
            JKRArchive::SDirEntry dir;

            if (!mArchive->getDirEntry(&dir, i)) {
                break;
            }

            if (((dir.mFileFlag >> JKRArchive::FILE_FLAG_FOLDER_SHIFT) & 1) != 0) {
                continue;
            }

            if (endsWithAnyExt(dir.mName, sLayoutExt) || endsWithAnyExt(dir.mName, sAnimationExt)) {
                continue;
            }

            void* pRes = mArchive->getIdxResource(i);

            if (pRes == nullptr) {
                PL_LOG_WARN("compat.layout", "LayoutHolder: failed to load '%s' (skipped)", dir.mName);
                continue;
            }

            mResOther.add(dir.mName, pRes, true);
        }
    }

    // Diagnostic: when the arc holds no brlyt at all, dump its entry table so
    // region-swapped archives (e.g. KrKorean's WiiRemoteStrapReplace) reveal
    // what they actually contain. Bounded so the log stays readable.
    if (mLayoutRes.mCount == 0) {
        const u32 kMaxDumpEntries = 64;
        u32 dumped = 0;

        PL_LOG_ERROR("compat.layout", "LayoutHolder: arc holds no brlyt; listing its entries:");

        for (u32 i = 0; dumped < kMaxDumpEntries; i++) {
            JKRArchive::SDirEntry dir;

            if (!mArchive->getDirEntry(&dir, i)) {
                break;
            }

            PL_LOG_ERROR("compat.layout", "  [%u] %s%s", static_cast< unsigned >(i),
                         ((dir.mFileFlag >> JKRArchive::FILE_FLAG_FOLDER_SHIFT) & 1) != 0 ? "<dir> " : "",
                         dir.mName != nullptr ? dir.mName : "?");
            dumped++;
        }
    }

    mount(nullptr);
}

void* LayoutHolder::GetResource(u32 type, const char* pName, u32* pSize) {
    if (pSize != nullptr) {
        *pSize = 0;
    }

    if (pName == nullptr) {
        return nullptr;
    }

    // 'timg' (0x74696D67) and 'font' (0x666F6E74) are the only resource types
    // nw4r lyt requests; both are served from the "other" table.
    void* pRes = mResOther.getRes(pName);

    if (pRes == nullptr) {
        char stripped[256];
        pRes = mResOther.getRes(stripExtInto(pName, stripped, sizeof(stripped)));
    }

    if (pRes == nullptr) {
        PL_LOG_WARN("compat.layout", "LayoutHolder::GetResource: type '%c%c%c%c' name '%s' not found in the arc",
                    static_cast< char >((type >> 24) & 0xFF), static_cast< char >((type >> 16) & 0xFF),
                    static_cast< char >((type >> 8) & 0xFF), static_cast< char >(type & 0xFF), pName);
    }

    return pRes;
}

nw4r::ut::Font* LayoutHolder::GetFont(const char* pName) {
    // SMG layout fonts come from the global font holder (/LayoutData/Font.arc,
    // loaded by GameSystemFontHolder at boot). The host holder is still a stub
    // (M9.5.3c wires text drawing), so report the miss once and return null;
    // TextBox then falls back to GetResource('font', name) for arcs that
    // bundle their own brfnt.
    static bool sLogged = false;

    if (!sLogged) {
        sLogged = true;
        PL_LOG_INFO("compat.layout", "LayoutHolder::GetFont('%s'): global font holder not wired yet (M9.5.3c)",
                    pName != nullptr ? pName : "(null)");
    }

    return nullptr;
}

void* LayoutHolder::getResOther(const char* pName) const {
    return mResOther.getRes(pName);
}

u32 LayoutHolder::getResOtherNum() const {
    return mResOther.mCount;
}

const char* LayoutHolder::getResOtherName(u32 idx) const {
    if (idx >= mResOther.mCount) {
        return nullptr;
    }

    return mResOther.getResName(idx);
}

void* LayoutHolder::getResOther(u32 idx) const {
    if (idx >= mResOther.mCount) {
        return nullptr;
    }

    return mResOther.getRes(idx);
}

bool LayoutHolder::isExistResOther(const char* pName) const {
    return mResOther.isExistRes(pName);
}

void LayoutHolder::mount(char*) {
    // The console version is only ever called as mount(nullptr) from
    // initializeArc (see the commented-out upstream body). No host behavior.
}
