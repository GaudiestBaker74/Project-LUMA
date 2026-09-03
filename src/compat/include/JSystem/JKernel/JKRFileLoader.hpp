#pragma once

// ============================================================================
// PC_PORT PATCH (compat/include overrides this header for the PC build).
//
// Change vs. upstream: `unsigned long` parameters became `u32` (Metrowerks
// PPC32 `long` == 32 bits; on LP64 hosts it is 64, which changes the mangled
// names and mismatches the definitions in the vendored .cpp files).
// Everything else is identical to upstream.
// ============================================================================

#include "JSystem/JKernel/JKRDisposer.hpp"

class JKRArcFinder;

class JKRFileLoader : public JKRDisposer {
public:
    JKRFileLoader();
    virtual ~JKRFileLoader();

    virtual void unmount();

    virtual bool becomeCurrent(const char*) = 0;
    virtual void* getResource(const char*) = 0;
    virtual void* getResource(u32, const char*) = 0;
    virtual u32 readResource(void*, u32, const char*) = 0;
    virtual u32 readResource(void*, u32, u32, const char*) = 0;
    virtual void removeResourceAll() = 0;
    virtual bool removeResource(void*) = 0;
    virtual bool detachResource(void*) = 0;
    virtual s32 getResSize(const void*) const = 0;
    virtual s32 countFile(const char*) const = 0;
    virtual JKRArcFinder* getFirstFile(const char*) const = 0;
    virtual u32 getExpandedResSize(const void*) const = 0;

    static void* getGlbResource(const char*, JKRFileLoader*);
    static void initializeVolumeList();
    void prependVolumeList(JSULink< JKRFileLoader >*);
    void removeVolumeList(JSULink< JKRFileLoader >*);

    static JSUList< JKRFileLoader > sFileLoaderList;  // 0x8060CF9C
    static JKRFileLoader* gCurrentFileLoader;         // 0x806B7140;

    static JSUList< JKRFileLoader > sVolumeList;

    JSULink< JKRFileLoader > mLoaderLink;  // 0x18
    char* mLoaderName;                     // 0x28
    u32 mLoaderType;                       // 0x2C
    bool mIsMounted;                       // 0x30
    u8 _31[3];
    u32 _34;
};
