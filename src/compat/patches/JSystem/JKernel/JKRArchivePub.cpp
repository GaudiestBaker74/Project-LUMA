// =============================================================================
// PC_PORT PATCH of the vendored JSystem/JKernel/JKRArchivePub.cpp (see
// patches/README.md).
//
// M9.5.1: the public JKRArchive API (mount + resource lookup/fetch).
//
// Changes vs. upstream:
//
// 1. `long`/`unsigned long` parameters became `s32`/`u32` to match the
//    compat/include shadow of JKRArchive.hpp (Metrowerks PPC32 `long` is
//    32-bit; on LP64 hosts the definitions would not match the declarations
//    and callers passing `&u32` where `unsigned long*` is declared fail to
//    compile).
//
// 2. `mount` only creates JKRMemArchive (MOUNT_MODE_MEM). The ARAM/DVD/COMP
//    modes reference JKRAramArchive/JKRDvdArchive/JKRCompArchive, which are
//    not compiled: ARAM does not exist on the host, and the DVD-mode archive
//    decompilation has known-bad sections (pointer-to-u32 casts in open()).
//    The game mounts everything through MR::mountArchive, which is MEM mode;
//    requesting another mode logs a warning and fails the mount instead of
//    failing the link.
//
// Everything else is identical to upstream.
// =============================================================================
#include "JSystem/JKernel/JKRArchive.hpp"
#include "JSystem/JKernel/JKRFileFinder.hpp"
#include "JSystem/JKernel/JKRHeap.hpp"
#include "JSystem/JKernel/JKRMemArchive.hpp"
#include "platform/Log/Log.h"
#include "revolution.h"

// PC_PORT: statics declared by the headers whose definitions the upstream
// decompilation does not include in any compiled TU (on console they live in
// JKernel's data section). Zero-init matches the console's BSS.
JKRFileLoader* JKRFileLoader::gCurrentFileLoader = nullptr;
JSUList< JKRFileLoader > JKRFileLoader::sVolumeList;
u32 JKRArchive::sCurrentDirID = 0;

bool JKRArchive::becomeCurrent(const char* pName) {
    SDIDirEntry* dir;

    if (*pName == '/') {
        const char* pDir = pName + 1;

        if (*pDir == 0) {
            pDir = nullptr;
        }

        dir = findDirectory(pDir, 0);
    } else {
        dir = findDirectory(pName, sCurrentDirID);
    }

    bool validDir = dir != nullptr;

    if (validDir) {
        JKRFileLoader::gCurrentFileLoader = this;
        sCurrentDirID = static_cast< u32 >(dir - mDirs);
    }

    return validDir;
}

void* JKRArchive::getResource(const char* pName) {
    SDIFileEntry* file;

    if (*pName == '/') {
        file = findFsResource(pName + 1, 0);
    } else {
        file = findFsResource(pName, sCurrentDirID);
    }

    if (file != nullptr) {
        return fetchResource(file, nullptr);
    }

    return nullptr;
}

void* JKRArchive::getResource(u32 a1, const char* pName) {
    SDIFileEntry* file;

    if (a1 == NULL_MAGIC || a1 == QUESTIONMARK_MAGIC) {
        file = findNameResource(pName);
    } else {
        file = findTypeResource(a1, pName);
    }

    if (file != nullptr) {
        return fetchResource(file, nullptr);
    }

    return nullptr;
}

u32 JKRArchive::readResource(void* a1, u32 a2, const char* pName) {
    SDIFileEntry* file;

    if (*pName == '/') {
        file = findFsResource(pName + 1, 0);
    } else {
        file = findFsResource(pName, sCurrentDirID);
    }

    if (file != nullptr) {
        u32 local_18;
        fetchResource(a1, a2, file, &local_18);

        return local_18;
    }

    return 0;
}

u32 JKRArchive::readResource(void* a1, u32 a2, u32 a3, const char* pName) {
    SDIFileEntry* file;

    if (a3 == NULL_MAGIC || a3 == QUESTIONMARK_MAGIC) {
        file = findNameResource(pName);
    } else {
        file = findTypeResource(a3, pName);
    }

    if (file != nullptr) {
        u32 local_18;
        fetchResource(a1, a2, file, &local_18);

        return local_18;
    }

    return 0;
}

void JKRArchive::removeResourceAll() {
    if (mInfoBlock != nullptr && mMountMode != MOUNT_MODE_MEM) {
        SDIFileEntry* current = mFiles;
        for (u32 i = 0; i < mInfoBlock->mNrFiles; i++) {
            if (current->mFileData != nullptr) {
                JKRHeap::free(current->mFileData, mHeap);
                current->mFileData = nullptr;
            }

            current++;
        }
    }
}

bool JKRArchive::removeResource(void* pResource) {
    SDIFileEntry* file = findPtrResource(pResource);

    if (file == nullptr) {
        return false;
    }

    file->mFileData = nullptr;
    JKRHeap::free(pResource, mHeap);

    return true;
}

bool JKRArchive::detachResource(void* pResource) {
    SDIFileEntry* file = findPtrResource(pResource);

    if (file == nullptr) {
        return false;
    }

    file->mFileData = nullptr;

    return true;
}

// PC_PORT (M9.5.1): the base getExpandedResSize is declared (and referenced
// by the JKRArchive vtable emitted from JKRArchivePri.cpp) but the
// decompilation provides no body. On the base class there is no compression
// knowledge, so it falls back to the raw resource size — every concrete
// archive subclass overrides it anyway.
u32 JKRArchive::getExpandedResSize(const void* pResource) const {
    return getResSize(pResource);
}

s32 JKRArchive::getResSize(const void* pResource) const {
    SDIFileEntry* file = findPtrResource(pResource);

    if (file == nullptr) {
        return -1;
    }

    return file->mDataSize;
}

s32 JKRArchive::countFile(const char* pName) const {
    SDIDirEntry* dir;

    if (*pName == '/') {
        pName++;

        if (*pName == 0) {
            pName = nullptr;
        }

        dir = findDirectory(pName, 0);
    } else {
        dir = findDirectory(pName, sCurrentDirID);
    }

    if (dir != nullptr) {
        return dir->mNrFiles;
    }

    return 0;
}

JKRArcFinder* JKRArchive::getFirstFile(const char* pName) const {
    SDIDirEntry* dir;

    if (*pName == '/') {
        pName++;

        if (*pName == 0) {
            pName = nullptr;
        }

        dir = findDirectory(pName, 0);
    } else {
        dir = findDirectory(pName, sCurrentDirID);
    }

    if (dir != nullptr) {
        // Bad to cast to non-const
        return new (JKRHeap::sGameHeap, 0) JKRArcFinder(const_cast< JKRArchive* >(this), dir->mFirstFileIndex, dir->mNrFiles);
    }

    return nullptr;
}

JKRArchive* JKRArchive::check_mount_already(s32 entryNum) {
    JSUPtrLink* current = JKRFileLoader::sFileLoaderList.mHead;

    while (current != nullptr) {
        JKRArchive* archive = reinterpret_cast< JKRArchive* >(current->mData);

        if (archive->mLoaderType == RARC_MAGIC && archive->mEntryNum == entryNum) {
            archive->_34++;
            return archive;
        }

        current = current->mNext;
    }

    return nullptr;
}

JKRArchive* JKRArchive::check_mount_already(s32 entryNum, JKRHeap* pHeap) {
    if (pHeap == nullptr) {
        pHeap = JKRHeap::sCurrentHeap;
    }

    JSUPtrLink* current = JKRFileLoader::sFileLoaderList.mHead;

    while (current != nullptr) {
        JKRArchive* archive = reinterpret_cast< JKRArchive* >(current->mData);

        if (archive->mLoaderType == RARC_MAGIC && archive->mEntryNum == entryNum && archive->mHeap == pHeap) {
            archive->_34++;
            return archive;
        }

        current = current->mNext;
    }

    return nullptr;
}

JKRArchive* JKRArchive::mount(const char* pName, EMountMode mountMode, JKRHeap* pHeap, EMountDirection mountDir) {
    s32 entryNum = DVDConvertPathToEntrynum(pName);

    if (entryNum < 0) {
        return nullptr;
    }

    return mount(entryNum, mountMode, pHeap, mountDir);
}

JKRArchive* JKRArchive::mount(s32 entryNum, EMountMode mountMode, JKRHeap* pHeap, EMountDirection mountDir) {
    JKRArchive* archive = check_mount_already(entryNum, pHeap);

    if (archive != nullptr) {
        return archive;
    }

    s32 uVar1 = -4;

    if (mountDir == MOUNT_DIRECTION_1) {
        uVar1 = 4;
    }

    switch (mountMode) {
    case MOUNT_MODE_MEM:
        archive = new (pHeap, uVar1) JKRMemArchive(entryNum, mountDir);
        break;
    // PC_PORT (M9.5.1): ARAM/DVD/COMP archive backends are not compiled
    // (no ARAM on the host; the DVD/COMP decompilations are not ported yet).
    // Fail the mount loudly instead of failing the link.
    default:
        PL_LOG_WARN("compat.rarc", "JKRArchive::mount: mode %d unsupported on host (MEM only)",
                    static_cast<int>(mountMode));
        return nullptr;
    }

    if (archive != nullptr && archive->mMountMode == MOUNT_MODE_0) {
        delete archive;
        archive = nullptr;
    }

    return archive;
}

bool JKRArchive::getDirEntry(SDirEntry* pDir, u32 fileIndex) const {
    SDIFileEntry* file = findIdxResource(fileIndex);

    if (file == nullptr) {
        return false;
    }

    pDir->mFileFlag = file->mFlag;
    pDir->mFileID = file->mFileID;
    pDir->mName = mStringTable + file->mNameOffset;

    return true;
}

void* JKRArchive::getIdxResource(u32 fileIndex) {
    SDIFileEntry* file = findIdxResource(fileIndex);

    if (file != nullptr) {
        return fetchResource(file, 0);
    }

    return nullptr;
}

void* JKRArchive::getResource(unsigned short fileID) {
    SDIFileEntry* file = findIdResource(fileID);

    if (file != nullptr) {
        return fetchResource(file, 0);
    }

    return nullptr;
}

u32 JKRArchive::readResource(void* pResource, u32 a2, unsigned short fileID) {
    SDIFileEntry* file = findIdResource(fileID);

    if (file != nullptr) {
        u32 local_18;
        fetchResource(pResource, a2, file, &local_18);

        return local_18;
    }

    return 0;
}

u32 JKRArchive::countResource() const {
    u32 count = 0;

    for (u32 i = 0; i < mInfoBlock->mNrFiles; i++) {
        if ((mFiles[i].mFlag & FILE_FLAG_FILE) != 0) {
            count++;
        }
    }

    return count;
}

u32 JKRArchive::getFileAttribute(u32 fileIndex) const {
    SDIFileEntry* file = findIdxResource(fileIndex);

    if (file != nullptr) {
        return file->mFlag;
    }

    return 0;
}
