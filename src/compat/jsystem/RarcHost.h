// compat/jsystem — RARC (JKRArchive) host conversion. See RarcHost.cpp for
// the full rationale (endianness + 64-bit pointer stride).
#pragma once

#include <JSystem/JKernel/JKRArchive.hpp>

#include <cstddef>
#include <cstdint>

namespace compat::rarc {

// True when `data` starts with a big-endian 'RARC' magic and is at least
// header-sized. Cheap pre-check before attempting a mount.
bool isRarc(const void* data, size_t size);

// Step 1: validates the blob and byte-swaps the header + info block in
// place. After this, `info->mNrFiles`/`mNrDirs` are host values — the caller
// uses mNrFiles to size the host file table for step 2.
// Returns false if the blob is not a consistent RARC (bad magic, truncated
// blob, section offsets out of range) — callers must treat that as a failed
// mount, not read the half-converted metadata.
bool convertHeaderInfo(u8* blob, u32 blobSize, JKRArchive::RarcHeader** outHeader, JKRArchive::RarcInfoBlock** outInfo);

// Step 2: byte-swaps the directory table in place (same 16-byte stride on
// disk and host) and rebuilds the file table from the 20-byte on-disk layout
// into the host `SDIFileEntry` layout (24 bytes: the trailing padding slot
// becomes the 64-bit mFileData pointer). `outHostFiles` must point to storage
// for at least `info->mNrFiles` entries (allocated on the mount heap).
// `blobSize` bounds-checks the tables against the loaded blob.
bool convertDirsAndFiles(u8* blob, u32 blobSize, JKRArchive::RarcInfoBlock* info, JKRArchive::SDIDirEntry** outDirs,
                         JKRArchive::SDIFileEntry* outHostFiles, char** outStringTable, u8** outFileDataStart);

// Decompressed size recorded in a Yaz0/Yay0 header (big-endian u32 at +4).
u32 expandSizeOf(const void* data);

}  // namespace compat::rarc
