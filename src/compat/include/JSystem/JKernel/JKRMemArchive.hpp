#pragma once

// ============================================================================
// PC_PORT PATCH (compat/include overrides this header for the PC build).
//
// Change vs. upstream: one extra member, `mHostFileEntries`.
//
// Why: the RARC on-disk file-entry table has a 20-byte stride (16 bytes of
// big-endian fields + a 4-byte padding slot the console runtime reused for
// the `mFileData` pointer — PPC32 pointers are 4 bytes). On a 64-bit host
// `sizeof(SDIFileEntry)` is 24, so `mFiles` cannot point into the loaded
// blob: every `mFiles[i]` would read the wrong entry. The patched
// JKRMemArchive::open therefore converts the disk table into a host-layout
// array (byte-swapped fields, native stride) allocated on the mount heap and
// points `mFiles` at it. This member owns that array (freed in the dtor).
//
// Everything else is identical to upstream.
// ============================================================================

#include "JSystem/JKernel/JKRArchive.hpp"

enum JKRMemBreakFlag { JKR_MEM_BREAK_FLAG_0 = 0, JKR_MEM_BREAK_FLAG_1 = 1 };

class JKRMemArchive : public JKRArchive {
public:
    JKRMemArchive();
    JKRMemArchive(s32, EMountDirection);
    virtual ~JKRMemArchive();

    virtual void removeResourceAll();
    virtual bool removeResource(void*);
    virtual u32 getExpandedResSize(const void*) const;
    virtual void* fetchResource(SDIFileEntry*, u32*);
    virtual void* fetchResource(void*, u32, SDIFileEntry*, u32*);

    void fixedInit(s32);
    bool mountFixed(void*, JKRMemBreakFlag);
    bool open(s32, EMountDirection);
    bool open(void*, u32, JKRMemBreakFlag);
    static s32 fetchResource_subroutine(unsigned char*, u32, unsigned char*, u32, int);

    RarcHeader* mHeader;  // 0x64
    u8* mFileDataStart;   // 0x68
    bool _6C;
    u8 _6D[3];

    // PC_PORT: host-layout copy of the on-disk file-entry table (see the
    // header comment). nullptr when nothing was converted/allocated.
    SDIFileEntry* mHostFileEntries;
};
