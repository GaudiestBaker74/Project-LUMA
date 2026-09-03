// compat/jsystem — RARC (JKRArchive) host conversion.
//
// Why this file exists
// --------------------
// The vendored JKernel archive code (JKRArchivePri/Pub, JKRMemArchive) reads
// RARC metadata straight out of the loaded blob through struct pointers:
//
//     mInfoBlock = (RarcInfoBlock*)(blob + headerSize);
//     mFiles     = (SDIFileEntry*)(info + info->mFileOffset);
//     ... mFiles[i].mNameOffset ...
//
// That is correct on the console for two reasons that do not hold on the PC:
//
//   1. **Endianness.** Every RARC field is big-endian; the Wii's PPC is
//      big-endian too, so a plain struct load reads the right value. On
//      x86-64/ARM64 the same load byteswaps it: `mNrFiles` becomes garbage,
//      name offsets point outside the string table, sizes explode. Nothing
//      in the vendored code converts, because on console nothing had to.
//
//   2. **Pointer width.** `SDIFileEntry` is 20 bytes on the console: 16
//      bytes of on-disk fields (12 bytes of data + 4 bytes of padding that
//      the runtime reuses for the `void* mFileData` cache — PPC32 pointers
//      are 4 bytes, so the struct strides the on-disk table exactly). On a
//      64-bit host `sizeof(SDIFileEntry)` is 24, so `mFiles` cannot point
//      into the blob at all — `mFiles[i]` would stride 24 bytes over a
//      20-byte table and read unrelated entries.
//
// So mounting an archive on the host needs a conversion step the console
// never performs: swap the fixed-size metadata in place, and rebuild the
// file-entry table in host layout. The patched JKRMemArchive::open calls
// convertHeaderInfo() + convertDirsAndFiles() right after the blob lands in
// RAM.
//
// Scope: the metadata (header, info block, directory table, file table) is
// converted. File *payloads* are left exactly as they are on disk — each
// consumer converts its own format (BTI/J3D/lyt/BNS) at parse time, which is
// also what the console code does.
#include "compat/jsystem/RarcHost.h"

#include "platform/Log/Log.h"

#include <cstring>

namespace {

inline u16 swap16(u16 v) {
    return static_cast< u16 >((v >> 8) | (v << 8));
}

inline u32 swap32(u32 v) {
    return ((v >> 24) & 0x000000FFu) | ((v >> 8) & 0x0000FF00u) | ((v << 8) & 0x00FF0000u) | ((v << 24) & 0xFF000000u);
}

// On-disk file entry: 20 bytes, big-endian (the console struct strides the
// table exactly because PPC32 pointers fit in the trailing padding slot).
// Kept separate from JKRArchive::SDIFileEntry because the host struct is 24
// bytes and its flag/name bitfield layout is compiler-defined.
struct RarcFileEntryOnDisk {
    u16 mFileID;
    u16 mHash;
    u8 mFlag;
    u8 mNameOffsetHi;  // top byte of the 24-bit name offset
    u16 mNameOffsetLo;
    u32 mDataOffset;  // or directory index for folder entries
    u32 mDataSize;
    u32 mPadding;  // the console runtime stores mFileData here
};

static_assert(sizeof(RarcFileEntryOnDisk) == 20, "RARC on-disk file entry must be 20 bytes");

}  // namespace

namespace compat::rarc {

bool isRarc(const void* data, size_t size) {
    if (data == nullptr || size < sizeof(JKRArchive::RarcHeader)) {
        return false;
    }
    const auto* bytes = static_cast< const u8* >(data);
    return bytes[0] == 'R' && bytes[1] == 'A' && bytes[2] == 'R' && bytes[3] == 'C';
}

bool convertHeaderInfo(u8* blob, u32 blobSize, JKRArchive::RarcHeader** outHeader,
                       JKRArchive::RarcInfoBlock** outInfo) {
    if (blob == nullptr || blobSize < sizeof(JKRArchive::RarcHeader)) {
        return false;
    }
    if (!isRarc(blob, blobSize)) {
        PL_LOG_WARN("compat.rarc", "not a RARC archive (bad magic)");
        return false;
    }

    auto* header = reinterpret_cast< JKRArchive::RarcHeader* >(blob);
    header->mMagic = swap32(header->mMagic);
    header->mFileSize = swap32(header->mFileSize);
    header->mHeaderSize = swap32(header->mHeaderSize);
    header->mFileDataOffset = swap32(header->mFileDataOffset);
    header->mTotalDataSize = swap32(header->mTotalDataSize);
    header->mMRamDataSize = swap32(header->mMRamDataSize);
    header->mARamDataSize = swap32(header->mARamDataSize);
    header->_1C = swap32(header->_1C);

    // The console trusts these offsets; the host must not, because a corrupt
    // or truncated archive would turn them into out-of-bounds reads.
    if (header->mHeaderSize < sizeof(JKRArchive::RarcHeader) || header->mHeaderSize > blobSize) {
        PL_LOG_WARN("compat.rarc", "header size %u out of range (blob %u)", header->mHeaderSize, blobSize);
        return false;
    }
    if (blobSize < header->mHeaderSize + sizeof(JKRArchive::RarcInfoBlock)) {
        PL_LOG_WARN("compat.rarc", "blob too small for the info block");
        return false;
    }

    auto* info = reinterpret_cast< JKRArchive::RarcInfoBlock* >(blob + header->mHeaderSize);
    info->mNrDirs = swap32(info->mNrDirs);
    info->mDirOffset = swap32(info->mDirOffset);
    info->mNrFiles = swap32(info->mNrFiles);
    info->mFileOffset = swap32(info->mFileOffset);
    info->mStringTableSize = swap32(info->mStringTableSize);
    info->mStringTableOffset = swap32(info->mStringTableOffset);
    info->mNextAvailableFileID = swap16(info->mNextAvailableFileID);
    info->mFileIDIsIndex = swap16(info->mFileIDIsIndex);
    info->_1C = swap32(info->_1C);

    if (info->mDirOffset > blobSize || info->mFileOffset > blobSize || info->mStringTableOffset > blobSize) {
        PL_LOG_WARN("compat.rarc", "section offsets out of range");
        return false;
    }

    *outHeader = header;
    *outInfo = info;
    return true;
}

bool convertDirsAndFiles(u8* blob, u32 blobSize, JKRArchive::RarcInfoBlock* info, JKRArchive::SDIDirEntry** outDirs,
                         JKRArchive::SDIFileEntry* outHostFiles, char** outStringTable, u8** outFileDataStart) {
    auto* header = reinterpret_cast< JKRArchive::RarcHeader* >(blob);
    u8* const infoBase = blob + header->mHeaderSize;

    // Bounds: the tables must live inside the loaded blob.
    const u64 dirBytes = static_cast< u64 >(info->mNrDirs) * sizeof(JKRArchive::SDIDirEntry);
    const u64 fileBytes = static_cast< u64 >(info->mNrFiles) * sizeof(RarcFileEntryOnDisk);
    if (info->mDirOffset + dirBytes > blobSize || info->mFileOffset + fileBytes > blobSize ||
        info->mStringTableOffset + info->mStringTableSize > blobSize) {
        PL_LOG_WARN("compat.rarc", "table extents out of range (dirs %u files %u)", info->mNrDirs, info->mNrFiles);
        return false;
    }
    if (header->mHeaderSize + header->mFileDataOffset > blobSize) {
        PL_LOG_WARN("compat.rarc", "file data start out of range");
        return false;
    }

    // Directory table: 16 bytes per entry, all fixed-size big-endian fields.
    // The host struct has the same size and alignment, so swap in place.
    auto* dirs = reinterpret_cast< JKRArchive::SDIDirEntry* >(infoBase + info->mDirOffset);
    for (u32 i = 0; i < info->mNrDirs; i++) {
        dirs[i].mID = swap32(dirs[i].mID);
        dirs[i].mNameOffset = swap32(dirs[i].mNameOffset);
        dirs[i].mHash = swap16(dirs[i].mHash);
        dirs[i].mNrFiles = swap16(dirs[i].mNrFiles);
        dirs[i].mFirstFileIndex = swap32(dirs[i].mFirstFileIndex);
    }

    // File table: convert from the 20-byte on-disk layout to the host struct.
    const u8* diskFiles = infoBase + info->mFileOffset;
    for (u32 i = 0; i < info->mNrFiles; i++) {
        RarcFileEntryOnDisk onDisk;
        std::memcpy(&onDisk, diskFiles + static_cast< size_t >(i) * sizeof(RarcFileEntryOnDisk), sizeof(onDisk));

        JKRArchive::SDIFileEntry& out = outHostFiles[i];
        out.mFileID = swap16(onDisk.mFileID);
        out.mHash = swap16(onDisk.mHash);
        out.mFlag = onDisk.mFlag;
        out.mNameOffset = (static_cast< u32 >(onDisk.mNameOffsetHi) << 16) | static_cast< u32 >(swap16(onDisk.mNameOffsetLo));
        out.mDataOffset = swap32(onDisk.mDataOffset);
        out.mDataSize = swap32(onDisk.mDataSize);
        out.mFileData = nullptr;  // filled lazily by fetchResource
    }

    *outDirs = dirs;
    *outStringTable = reinterpret_cast< char* >(infoBase + info->mStringTableOffset);
    *outFileDataStart = blob + header->mHeaderSize + header->mFileDataOffset;
    return true;
}

u32 expandSizeOf(const void* data) {
    // Yaz0/Yay0 store the decompressed size big-endian at offset 4.
    const auto* bytes = static_cast< const u8* >(data);
    return (static_cast< u32 >(bytes[4]) << 24) | (static_cast< u32 >(bytes[5]) << 16) |
           (static_cast< u32 >(bytes[6]) << 8) | static_cast< u32 >(bytes[7]);
}

}  // namespace compat::rarc
