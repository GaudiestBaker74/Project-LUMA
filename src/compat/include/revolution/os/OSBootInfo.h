#ifndef OSBOOTINFO_H
#define OSBOOTINFO_H

// ============================================================================
// PC_PORT PATCH (compat/include overrides this header for the PC build).
//
// Change vs. upstream: the member declaration uses the *struct tag*
// `struct DVDDiskID` instead of the typedef `DVDDiskID`.
//
// Upstream declares `DVDDiskID DVDDiskID;` — a member whose name shadows the
// typedef in scope. mwcc accepts this, but GCC/Clang reject it in C++ with
// "changes meaning of 'DVDDiskID'". Using the elaborated type specifier keeps
// the same member name, same layout, and compiles cleanly on host toolchains.
//
// Only `memorySize` of this struct is read by the game code we compile
// (JKRHeap::initArena); the struct layout is preserved regardless.
// ============================================================================

#include <revolution/dvd.h>

typedef struct OSBootInfo_s {
    struct DVDDiskID DVDDiskID;
    u32 magic;
    u32 version;
    u32 memorySize;
    u32 consoleType;
    void* arenaLo;
    void* arenaHi;
    void* FSTLocation;
    u32 FSTMaxLength;
} OSBootInfo;

#endif // OSBOOTINFO_H
