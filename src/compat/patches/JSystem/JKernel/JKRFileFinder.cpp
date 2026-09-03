// =============================================================================
// PC_PORT PATCH of the vendored JSystem/JKernel/JKRFileFinder.cpp (see
// patches/README.md).
//
// Change vs. upstream: adds the body of `JKRFileFinder::~JKRFileFinder()`,
// which the decompilation declares but never defines — without it the
// JKRFileFinder vtable has an undefined reference and the link fails. The
// original destructor is trivial (no owned resources).
//
// Everything else is identical to upstream.
// =============================================================================
#include "JSystem/JKernel/JKRFileFinder.hpp"
#include "JSystem/JKernel/JKRArchive.hpp"

JKRFileFinder::JKRFileFinder() {
    mHasMoreFiles = false;
    mFileIsFolder = false;
}

// PC_PORT (M9.5.1): the base destructor is declared by the header but the
// decompilation provides no body for it, so the vtable emitted here has an
// undefined reference and nothing links. It is trivial in the original
// (no members to release); the derived ~JKRArcFinder below is what the
// decomp did recover.
JKRFileFinder::~JKRFileFinder() {
}

JKRArcFinder::JKRArcFinder(JKRArchive* pArchive, long firstFileIndex, long nrFiles) {
    mArchive = pArchive;
    mHasMoreFiles = nrFiles > 0;
    mFirstIndex = firstFileIndex;
    mLastIndex = firstFileIndex + nrFiles - 1;
    mCurrentIndex = firstFileIndex;

    findNextFile();
}

// Looks identical to base destructor, does not call ~JKRFileFinder()
JKRArcFinder::~JKRArcFinder() {
}

bool JKRArcFinder::findNextFile() {
    if (mHasMoreFiles) {
        bool moreFiles = mCurrentIndex <= mLastIndex;
        mHasMoreFiles = moreFiles;

        // Weird code
        if (moreFiles & 0xFF) {
            JKRArchive::SDirEntry dir;
            mHasMoreFiles = mArchive->getDirEntry(&dir, mCurrentIndex);

            mName = dir.mName;
            mDirIndex = mCurrentIndex;
            mFileID = dir.mFileID;
            mFileFlag = dir.mFileFlag;
            mFileIsFolder = ((mFileFlag >> JKRArchive::FILE_FLAG_FOLDER_SHIFT) & 1) != 0;

            mCurrentIndex++;
        }
    }

    return mHasMoreFiles;
}
