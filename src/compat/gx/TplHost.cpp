// =============================================================================
// compat/gx — TPL big-endian -> host conversion. See TplHost.h for the model.
//
// On-disk (big-endian) layout, from the RVL SDK TPL format:
//   TPLPalette  @0: u32 versionNumber (0x0020AF30), u32 numDescriptors,
//                   u32 descriptorArrayOffset (relative to the file start)
//   TPLDescriptor @0: u32 textureHeaderOffset, u32 clutHeaderOffset (both
//                   relative to the file start; 0 = absent)
//   TPLHeader   @0: u16 height, u16 width, u32 format, u32 dataOffset,
//                   u32 wrapS, u32 wrapT, u32 minFilter, u32 magFilter,
//                   f32 LODBias, u8 edgeLODEnable, u8 minLOD, u8 maxLOD,
//                   u8 unpacked
//   TPLClutHeader @0: u16 numEntries, u8 unpacked, u8 _4, u32 format,
//                   u32 dataOffset
//
// The vendored TPLBind() resolves those offsets in place with `(u32)ptr`
// arithmetic, which only works on a 32-bit big-endian target. Everything else
// about this file is new host code.
// =============================================================================
#include "compat/gx/TplHost.h"

#include "platform/Log/Log.h"

#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

constexpr u32 kTplVersion = 0x0020AF30;

u16 readBE16(const u8* p) {
    return static_cast<u16>((static_cast<u32>(p[0]) << 8) | p[1]);
}

u32 readBE32(const u8* p) {
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
           (static_cast<u32>(p[2]) << 8) | static_cast<u32>(p[3]);
}

f32 readBEf32(const u8* p) {
    const u32 bits = readBE32(p);
    f32 value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// One converted palette plus the single allocation that backs its structs.
struct HostTpl {
    TPLPalette palette;
    std::vector<u8> storage;  // descriptors + headers + clut headers
};

// Source blob -> conversion. Meyers singleton: no namespace-scope dynamic
// initialization order to trip over.
std::unordered_map<void*, HostTpl*>& tplCache() {
    static std::unordered_map<void*, HostTpl*> cache;
    return cache;
}

std::mutex& tplCacheMutex() {
    static std::mutex mutex;
    return mutex;
}

// True when `p` still starts with the big-endian TPL version word (guards the
// cache against a recycled source address).
bool isBeTpl(const u8* p) {
    return p != nullptr && readBE32(p) == kTplVersion;
}

HostTpl* convert(const u8* src) {
    const u32 numDescriptors = readBE32(src + 4);
    const u32 descArrayOff = readBE32(src + 8);

    // Sanity caps: a layout TPL holds a handful of textures. The descriptor
    // array must also fit in a plausible file (we do not know the real blob
    // size here, so only guard against absurd values).
    if (numDescriptors == 0 || numDescriptors > 4096) {
        PL_LOG_WARN("compat.tpl", "TPL: implausible descriptor count %u", numDescriptors);
        return nullptr;
    }
    if (descArrayOff < 12) {
        PL_LOG_WARN("compat.tpl", "TPL: descriptor array offset %u before the header end",
                    descArrayOff);
        return nullptr;
    }

    // On-disk element sizes (BE, 32-bit offset fields).
    constexpr u32 kDescSize = 8;

    HostTpl* host = new HostTpl();
    // Native element sizes (host pointers): descriptors first, then one
    // texture header + one optional CLUT header each. All native structs are
    // 8-aligned and their sizeofs are multiples of 8, so the sequential
    // bump allocation below keeps every member aligned.
    static_assert(sizeof(TPLDescriptor) % 8 == 0, "TPL alignment assumption");
    static_assert(sizeof(TPLHeader) % 8 == 0, "TPL alignment assumption");
    static_assert(sizeof(TPLClutHeader) % 8 == 0, "TPL alignment assumption");
    const u64 storageBytes =
        static_cast<u64>(numDescriptors) *
        (sizeof(TPLDescriptor) + sizeof(TPLHeader) + sizeof(TPLClutHeader));
    host->storage.assign(static_cast<size_t>(storageBytes), 0);

    auto* descriptors = reinterpret_cast<TPLDescriptor*>(host->storage.data());
    u8* cursor = host->storage.data() + numDescriptors * sizeof(TPLDescriptor);

    for (u32 i = 0; i < numDescriptors; ++i) {
        const u8* desc = src + descArrayOff + i * kDescSize;
        const u32 texOff = readBE32(desc + 0);
        const u32 clutOff = readBE32(desc + 4);

        if (texOff != 0) {
            const u8* th = src + texOff;
            auto* header = reinterpret_cast<TPLHeader*>(cursor);
            cursor += sizeof(TPLHeader);

            header->height = readBE16(th + 0);
            header->width = readBE16(th + 2);
            header->format = readBE32(th + 4);
            // Payloads stay in the source blob: raw tiled texels need no swap.
            const u32 dataOff = readBE32(th + 8);
            header->data = dataOff != 0 ? reinterpret_cast<char*>(const_cast<u8*>(src) + dataOff)
                                        : nullptr;
            header->wrapS = static_cast<GXTexWrapMode>(readBE32(th + 12));
            header->wrapT = static_cast<GXTexWrapMode>(readBE32(th + 16));
            header->minFilter = static_cast<GXTexFilter>(readBE32(th + 20));
            header->magFilter = static_cast<GXTexFilter>(readBE32(th + 24));
            header->LODBias = readBEf32(th + 28);
            header->edgeLODEnable = th[32];
            header->minLOD = th[33];
            header->maxLOD = th[34];
            header->unpacked = 1;  // offsets already resolved

            descriptors[i].textureHeader = header;
        } else {
            descriptors[i].textureHeader = nullptr;
        }

        if (clutOff != 0) {
            const u8* ch = src + clutOff;
            auto* clut = reinterpret_cast<TPLClutHeader*>(cursor);
            cursor += sizeof(TPLClutHeader);

            clut->numEntries = readBE16(ch + 0);
            clut->unpacked = 1;
            clut->_4 = ch[3];
            clut->format = static_cast<GXTlutFmt>(readBE32(ch + 4));
            const u32 dataOff = readBE32(ch + 8);
            clut->data = dataOff != 0 ? reinterpret_cast<char*>(const_cast<u8*>(src) + dataOff)
                                      : nullptr;

            descriptors[i].CLUTHeader = clut;
        } else {
            descriptors[i].CLUTHeader = nullptr;
        }
    }

    host->palette.versionNumber = kTplVersion;
    host->palette.numDescriptors = numDescriptors;
    host->palette.descriptorArray = descriptors;
    return host;
}

} // namespace

namespace Platform::CompatGx {

TPLPalettePtr tplToHost(void* tplData) {
    u8* src = static_cast<u8*>(tplData);
    if (!isBeTpl(src)) {
        PL_LOG_WARN("compat.tpl", "tplToHost: not a big-endian TPL (%p)", tplData);
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(tplCacheMutex());
    auto& cache = tplCache();
    const auto it = cache.find(tplData);
    if (it != cache.end() && isBeTpl(src)) {
        return &it->second->palette;
    }

    HostTpl* host = convert(src);
    if (host == nullptr) {
        return nullptr;
    }
    cache[tplData] = host;  // intentionally never freed (process-lifetime)
    PL_LOG_INFO("compat.tpl", "TPL converted: %u descriptor(s) from %p",
                static_cast<unsigned>(host->palette.numDescriptors), tplData);
    return &host->palette;
}

} // namespace Platform::CompatGx

// --- Vendored TPL API (revolution/tpl.h) ------------------------------------
// The decompiled RVL TPL.c is not compiled (its TPLBind is 32-bit BE in-place
// offset resolution). The host contract: TPLPalette instances handed to the
// game are always native trees produced by tplToHost(), so TPLGet is a plain
// index and TPLBind is a no-op.

extern "C" {

void TPLBind(TPLPalettePtr ptr) {
    // Native palettes are bound at conversion time. A palette that is not
    // native cannot be bound in place on the host (4-byte offset fields vs.
    // 8-byte pointers) — callers must go through
    // Platform::CompatGx::tplToHost.
    if (ptr != nullptr && ptr->versionNumber != kTplVersion) {
        PL_LOG_WARN("compat.tpl", "TPLBind on a non-native palette ignored (use tplToHost)");
    }
}

TPLDescriptorPtr TPLGet(TPLPalettePtr ptr, u32 id) {
    if (ptr == nullptr || ptr->numDescriptors == 0 || ptr->descriptorArray == nullptr) {
        PL_LOG_WARN("compat.tpl", "TPLGet on an empty palette");
        return nullptr;
    }
    return &ptr->descriptorArray[id % ptr->numDescriptors];
}

} // extern "C"
