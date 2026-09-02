#pragma once
// ============================================================================
// PC_PORT PATCH (compat/include override, wins over the vendored headers).
//
// The vendored Game/Screen/IsbnManager.hpp includes <nw4r/lyt/drawInfo.h>,
// whose nw4r math headers contain Metrowerks inline asm (fsel/ps_*) that does
// not compile with a host toolchain. The ISBN layout is the Chinese-region
// logo censorship screen: the host boot runs with region "US" and never
// creates it, so only the class surface the LogoScene links against is kept
// (the real layout tree returns with the nw4r layout milestone, M9.5+).
// ============================================================================

#include <revolution/mem.h>

namespace nw4r {
    namespace lyt {
        class Layout;
        class ArcResourceAccessor;
    }; // namespace lyt
}; // namespace nw4r

class IsbnManager {
private:
    /* 0x00 */ bool _0;
    /* 0x04 */ MEMAllocator* mpAllocator;
    /* 0x08 */ void* mpLayout;      // nw4r::lyt::Layout* (not host-compiled yet)
    /* 0x0C */ void* mpResAccessor; // nw4r::lyt::ArcResourceAccessor*
    /* 0x10 */ char mDrawInfoStorage[0x60]; // nw4r::lyt::DrawInfo (opaque on host)

public:
    /// @brief Creates a new `IsbnManager`.
    /// @param pAllocator A pointer to the memory allocator.
    IsbnManager(MEMAllocator* pAllocator);

    /// @brief Destroys the `IsbnManager`.
    virtual ~IsbnManager();

    void setAdjustRate(f32, f32);
    void setNumber(const wchar_t* pIsbnNumber, const wchar_t* pRegistNumber, const wchar_t* pOtherNumber);
    void calculateView();
    static IsbnManager* create(void* pArchiveBuf, MEMAllocator* pAllocator);
    bool calc(bool);
    void draw();
    void reset();
};
