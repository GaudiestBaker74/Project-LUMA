// =============================================================================
// PC_PORT PATCH of the vendored JSystem/JKernel/JKRDvdFile.cpp (see
// patches/README.md).
//
// M9.5.1: the DVD-file wrapper JKRDvdRipper/JKRMemArchive read through.
//
// Change vs. upstream: `doneProcess` recovered the owning JKRDvdFile with
// `*(JKRDvdFile**)((u8*)fileInfo + 0x3c)` — a hardcoded PPC32 struct offset
// into DVDFileInfo (the console SDK stored the back-pointer there). The host
// DVDFileInfo (compat/dvd) has a different layout and uses cb.userData for
// the FST entrynum, so that read lands in the middle of the command block
// and yields garbage → the completion message goes to a random address.
// On the host the owner is found by walking sDvdList (open() appends every
// live JKRDvdFile to it) and comparing &mFileInfo. O(n) over a handful of
// files, on the DVD worker thread only.
//
// Everything else is identical to upstream.
// =============================================================================
#include "JSystem/JKernel/JKRDvdFile.hpp"
#include "JSystem/JUtility/JUTException.hpp"
#include <stdint.h>

JSUList< JKRDvdFile > JKRDvdFile::sDvdList;

JKRDvdFile::JKRDvdFile() : mDvdLink(this) {
    initiate();
}

JKRDvdFile::JKRDvdFile(s32 entryNum) : mDvdLink(this) {
    initiate();
    mIsAvailable = open(entryNum);
    if (!mIsAvailable) {
        return;
    } else {
        return;
    }
}

JKRDvdFile::~JKRDvdFile() {
    close();
}

void JKRDvdFile::initiate(void) {
    mDvdFile = this;
    OSInitMutex(&mMutex1);
    OSInitMutex(&mMutex2);
    OSInitMessageQueue(&mMessageQueue2, &mMessage2, 1);
    OSInitMessageQueue(&mMessageQueue1, &mMessage1, 1);
    mOSThread = NULL;
    field_0x50 = 0;
    field_0x58 = 0;
}

bool JKRDvdFile::open(const char* name) {
    if (!mIsAvailable) {
        mIsAvailable = DVDOpen(name, &mFileInfo);
        if (mIsAvailable) {
            sDvdList.append(&mDvdLink);
            getStatus();
        }
    }
    return mIsAvailable;
}

bool JKRDvdFile::open(s32 entryNum) {
    if (!mIsAvailable) {
        mIsAvailable = DVDFastOpen(entryNum, &mFileInfo);
        if (mIsAvailable) {
            sDvdList.append(&mDvdLink);
            getStatus();
        }
    }
    return mIsAvailable;
}

void JKRDvdFile::close() {
    if (mIsAvailable) {
        if (DVDClose(&mFileInfo) != 0) {
            mIsAvailable = false;
            sDvdList.remove(&mDvdLink);
        } else {
            JUTException::panic(__FILE__, 213, "cannot close DVD file\n");
        }
    }
}

s32 JKRDvdFile::readData(void* param_1, s32 length, s32 param_3) {
    OSLockMutex(&mMutex1);
    if (mOSThread) {
        OSUnlockMutex(&mMutex1);
        return -1;
    }

    mOSThread = OSGetCurrentThread();

    s32 result = -1;
    if (DVDReadAsyncPrio(&mFileInfo, param_1, length, param_3, JKRDvdFile::doneProcess, 2)) {
        result = sync();
    }

    mOSThread = NULL;
    OSUnlockMutex(&mMutex1);

    return result;
}

s32 JKRDvdFile::writeData(void const* param_0, s32 length, s32 param_2) {
    return -1;
}

s32 JKRDvdFile::sync(void) {
    OSMessage message;
    OSLockMutex(&mMutex1);
    OSReceiveMessage(&mMessageQueue2, &message, 1);
    mOSThread = NULL;
    OSUnlockMutex(&mMutex1);
    return (intptr_t)message;
}

void JKRDvdFile::doneProcess(s32 id, DVDFileInfo* fileInfo) {
    // PC_PORT: the upstream `*(JKRDvdFile**)((u8*)fileInfo + 0x3c)` reads a
    // back-pointer at a hardcoded PPC32 offset inside DVDFileInfo. The host
    // DVDFileInfo has a different layout (and compat/dvd uses cb.userData for
    // the FST entrynum), so find the owner through sDvdList instead — every
    // open JKRDvdFile is on it, and the list holds a handful of entries.
    JKRDvdFile* dvdFile = nullptr;
    for (JSUPtrLink* link = sDvdList.mHead; link != nullptr; link = link->mNext) {
        auto* candidate = reinterpret_cast< JKRDvdFile* >(link->mData);
        if (candidate->getFileInfo() == fileInfo) {
            dvdFile = candidate;
            break;
        }
    }
    if (dvdFile == nullptr) {
        return;
    }

    OSSendMessage(&dvdFile->mMessageQueue2, (OSMessage)(intptr_t)id, OS_MESSAGE_NOBLOCK);
}

JKRFile::JKRFile() : JKRDisposer() {
    mIsAvailable = false;
}

s32 JKRDvdFile::getFileSize(void) const {
    return mFileInfo.length;
}
