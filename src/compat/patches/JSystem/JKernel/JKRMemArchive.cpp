// =============================================================================
// PC_PORT PATCH of the vendored JSystem/JKernel/JKRMemArchive.cpp (see
// patches/README.md).
//
// M9.5.1: the in-memory RARC archive — what MR::mountArchive() creates and
// what the whole resource pipeline (layouts, models, messages) reads through.
//
// Changes vs. upstream:
//
// 1. **RARC host conversion.** Both open() overloads pointed mInfoBlock /
//    mDirs / mFiles / mStringTable straight into the loaded blob. On the
//    console that works (big-endian host, 4-byte pointers); on the PC every
//    metadata field is byteswapped and the 20-byte on-disk file entries do
//    not match the 24-byte host SDIFileEntry (see compat/jsystem/RarcHost.h
//    for the full rationale). After the blob is loaded, the metadata is now
//    converted: header/info/dirs byte-swapped in place, file table rebuilt
//    into a host-layout array allocated on the mount heap (mHostFileEntries,
//    the new member in the compat/include override of JKRMemArchive.hpp).
//    A blob that fails validation is treated as a failed mount
//    (mMountMode = MOUNT_MODE_0) instead of being read blindly.
//
// 2. `open(long, EMountDirection)` dropped the MOUNT_DIRECTION_BACKWARD
//    duplicate of the same loadToMainRAM call — the two branches differed
//    only in the (unused on host) alloc direction; kept for clarity.
//
// 3. `mountFixed`: `check_mount_already(reinterpret_cast<s32>(ptr))` and
//    `fixedInit(reinterpret_cast<s32>(ptr))` truncated a pointer to s32.
//    Kept (mEntryNum is s32 by class layout and the value is only compared
//    against itself), but through uintptr_t first — the direct
//    pointer→smaller-int cast is a hard error on some host compilers.
//
// Everything else (fetchResource, fetchResource_subroutine, removeResource…)
// is identical to upstream.
// =============================================================================
#include "JSystem/JKernel/JKRMemArchive.hpp"
#include "JSystem/JKernel/JKRDecomp.hpp"
#include "JSystem/JKernel/JKRDvdRipper.hpp"
#include "JSystem/JKernel/JKRHeap.hpp"
#include "JSystem/JUtility/JUTException.hpp"
#include "compat/jsystem/RarcHost.h"
#include "revolution.h"
#include <cstdint>
#include <cstring>

// PC_PORT: converts the freshly loaded RARC blob and fills the metadata
// pointers. Returns false when the blob is not a mountable RARC.
static bool PC_PORT_convertRarc(JKRMemArchive* self, u8* blob, u32 size) {
    JKRArchive::RarcHeader* header = nullptr;
    JKRArchive::RarcInfoBlock* info = nullptr;
    if (!compat::rarc::convertHeaderInfo(blob, size, &header, &info)) {
        return false;
    }

    // Host-layout file table on the mount heap (see RarcHost.h: the on-disk
    // 20-byte stride does not match the 64-bit host struct).
    JKRArchive::SDIFileEntry* hostFiles = nullptr;
    if (info->mNrFiles > 0) {
        hostFiles = static_cast< JKRArchive::SDIFileEntry* >(
            JKRHeap::alloc(static_cast< u32 >(info->mNrFiles * sizeof(JKRArchive::SDIFileEntry)), 4, self->mHeap));
        if (hostFiles == nullptr) {
            return false;
        }
        std::memset(hostFiles, 0, info->mNrFiles * sizeof(JKRArchive::SDIFileEntry));
    }

    JKRArchive::SDIDirEntry* dirs = nullptr;
    char* stringTable = nullptr;
    u8* fileDataStart = nullptr;
    if (!compat::rarc::convertDirsAndFiles(blob, size, info, &dirs, hostFiles, &stringTable, &fileDataStart)) {
        if (hostFiles != nullptr) {
            JKRHeap::free(hostFiles, self->mHeap);
        }
        return false;
    }

    self->mHeader = header;
    self->mInfoBlock = info;
    self->mDirs = dirs;
    self->mFiles = hostFiles;
    self->mHostFileEntries = hostFiles;
    self->mStringTable = stringTable;
    self->mFileDataStart = fileDataStart;
    return true;
}

JKRMemArchive::JKRMemArchive() {
    mHostFileEntries = nullptr;
}

JKRMemArchive::JKRMemArchive(s32 entryNum, EMountDirection mountDir) : JKRArchive(entryNum, MOUNT_MODE_MEM) {
    mIsMounted = false;
    mMountDir = mountDir;
    mHostFileEntries = nullptr;

    if (!open(entryNum, mountDir)) {
        return;
    }

    mLoaderType = RARC_MAGIC;
    mLoaderName = mStringTable + mDirs->mNameOffset;

    prependVolumeList(&mLoaderLink);

    mIsMounted = true;
}

JKRMemArchive::~JKRMemArchive() {
    if (mIsMounted == true) {
        if (mHostFileEntries != nullptr && mHeap != nullptr) {
            JKRHeap::free(mHostFileEntries, mHeap);
            mHostFileEntries = nullptr;
        }

        if (_6C && mHeader != nullptr) {
            JKRHeap::free(mHeader, mHeap);
        }

        removeVolumeList(&mLoaderLink);
        mIsMounted = false;
    }
}

void JKRMemArchive::removeResourceAll() {
    if (mInfoBlock == nullptr) {
        return;
    }

    if (mMountMode == MOUNT_MODE_MEM) {
        return;
    }

    SDIFileEntry* current = mFiles;

    for (s32 i = 0; i < mInfoBlock->mNrFiles; i++) {
        if (current->mFileData != nullptr) {
            current->mFileData = nullptr;
        }
    }
}

bool JKRMemArchive::removeResource(void* pResource) {
    SDIFileEntry* file = findPtrResource(pResource);

    if (file == nullptr) {
        return false;
    }

    file->mFileData = nullptr;
    return true;
}

u32 JKRMemArchive::getExpandedResSize(const void* pResource) const {
    SDIFileEntry* file = findPtrResource(pResource);

    if (file == nullptr) {
        return -1;
    }

    if ((file->mFlag & FILE_FLAG_COMPRESSED) == 0) {
        return getResSize(pResource);
    }

    return JKRDecompExpandSize(reinterpret_cast< u8* >(const_cast< void* >(pResource)));
}

void* JKRMemArchive::fetchResource(SDIFileEntry* pFile, u32* pSize) {
    if (pFile->mFileData == nullptr) {
        pFile->mFileData = mFileDataStart + pFile->mDataOffset;
    }

    if (pSize != nullptr) {
        *pSize = pFile->mDataSize;
    }

    return pFile->mFileData;
}

void* JKRMemArchive::fetchResource(void* pData, u32 dataSize, SDIFileEntry* pFile, u32* pSize) {
    u32 size = pFile->mDataSize;

    if (size > dataSize) {
        size = dataSize;
    }

    if (pFile->mFileData != nullptr) {
        memcpy(pData, pFile->mFileData, size);
    } else {
        s32 compression;

        if ((pFile->mFlag & FILE_FLAG_COMPRESSED) == 0) {
            compression = JKR_COMPRESSION_NONE;
        } else if ((pFile->mFlag & FILE_FLAG_IS_YAZ0) != 0) {
            compression = JKR_COMPRESSION_SZS;
        } else {
            compression = JKR_COMPRESSION_SZP;
        }

        size = fetchResource_subroutine(mFileDataStart + pFile->mDataOffset, size, reinterpret_cast< u8* >(pData), dataSize, compression);
    }

    if (pSize != nullptr) {
        *pSize = size;
    }

    return pData;
}

void JKRMemArchive::fixedInit(s32 entryNum) {
    mIsMounted = false;
    mMountMode = MOUNT_MODE_MEM;
    _34 = 1;
    _58 = 2;
    mHeap = JKRHeap::sCurrentHeap;
    mEntryNum = entryNum;

    if (gCurrentFileLoader != nullptr) {
        return;
    }

    gCurrentFileLoader = this;
    sCurrentDirID = 0;
}

bool JKRMemArchive::mountFixed(void* a1, JKRMemBreakFlag breakFlag) {
    if (check_mount_already(static_cast< s32 >(reinterpret_cast< uintptr_t >(a1))) != nullptr) {
        return false;
    }

    fixedInit(static_cast< s32 >(reinterpret_cast< uintptr_t >(a1)));

    if (!open(a1, 0xFFFF, breakFlag)) {
        return false;
    }

    SDIDirEntry* firstDir = mDirs;
    char* stringTable = mStringTable;

    mLoaderType = RARC_MAGIC;
    mLoaderName = stringTable + firstDir->mNameOffset;

    prependVolumeList(&mLoaderLink);

    mIsMounted = true;
    _6C = breakFlag == JKR_MEM_BREAK_FLAG_1;

    return true;
}

bool JKRMemArchive::open(s32 entryNum, EMountDirection mountDir) {
    mHeader = nullptr;
    mInfoBlock = nullptr;
    mFileDataStart = nullptr;
    mDirs = nullptr;
    mFiles = nullptr;
    mStringTable = nullptr;
    mHostFileEntries = nullptr;
    _6C = false;
    mMountDir = mountDir;

    // PC_PORT: the alloc direction only mattered for the console's MEM2
    // layout; both upstream branches made the identical loadToMainRAM call
    // otherwise.
    u32 size = 0;
    void* pData = JKRDvdRipper::loadToMainRAM(static_cast< s32 >(entryNum), nullptr, EXPAND_SWITCH_UNKNOWN1, 0, mHeap,
                                              JKRDvdRipper::ALLOC_DIRECTION_FORWARD, 0, reinterpret_cast< int* >(&_5C),
                                              &size);
    if (pData == nullptr) {
        mMountMode = MOUNT_MODE_0;
        return false;
    }

    mHeader = reinterpret_cast< RarcHeader* >(pData);
    DCInvalidateRange(pData, size);

    // PC_PORT (M9.5.1): big-endian metadata + 20-byte on-disk file entries do
    // not work through struct pointers on the host — convert (RarcHost.h).
    // On failure the ripper-allocated blob is freed here (the destructor's
    // _6C guard never sees it: _6C only becomes true on success).
    if (!PC_PORT_convertRarc(this, reinterpret_cast< u8* >(pData), size)) {
        JKRHeap::free(pData, mHeap);
        mHeader = nullptr;
        mMountMode = MOUNT_MODE_0;
        return false;
    }
    _6C = true;

    return mMountMode != MOUNT_MODE_0;
}

bool JKRMemArchive::open(void* pData, u32 a2, JKRMemBreakFlag breakFlag) {
    mHeader = reinterpret_cast< RarcHeader* >(pData);
    mInfoBlock = nullptr;
    mFileDataStart = nullptr;
    mDirs = nullptr;
    mFiles = nullptr;
    mStringTable = nullptr;
    mHostFileEntries = nullptr;
    mHeap = JKRHeap::findFromRoot(pData);
    _5C = 0;

    // PC_PORT (M9.5.1): same conversion as the DVD-backed open() above. The
    // blob is the caller's (mountFixed semantics), so on failure nothing is
    // freed here.
    if (!PC_PORT_convertRarc(this, reinterpret_cast< u8* >(pData), static_cast< u32 >(a2))) {
        mHeader = nullptr;
        mMountMode = MOUNT_MODE_0;
        return false;
    }
    _6C = breakFlag == JKR_MEM_BREAK_FLAG_1;

    return true;
}

// Register mismatch
s32 JKRMemArchive::fetchResource_subroutine(unsigned char* pSrc, u32 srcSize, unsigned char* pDst, u32 dstSize, int compression) {
    switch (compression) {
    case JKR_COMPRESSION_NONE:
        if (srcSize > dstSize) {
            srcSize = dstSize;
        }

        memcpy(pDst, pSrc, srcSize);

        return srcSize;
    case JKR_COMPRESSION_SZP:
    case JKR_COMPRESSION_SZS:
        srcSize = JKRDecompExpandSize(pSrc);

        if (srcSize > dstSize) {
            srcSize = dstSize;
        }

        JKRDecomp::orderSync(pSrc, pDst, srcSize, 0);
        return srcSize;
    default:
        JUTException::panic_f(__FILE__, 723, "%s", "??? bad sequence\n");
        break;
    }

    return 0;
}
