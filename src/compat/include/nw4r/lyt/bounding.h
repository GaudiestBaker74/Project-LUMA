#pragma once

// =============================================================================
// PC_PORT SHADOW of the vendored nw4r/lyt/bounding.h (M9.5.3a, Windows fix).
//
// Only change vs. upstream: the forward declaration of ResBlockSet is
// `struct`, not `class`. ResBlockSet is DEFINED as a struct (nw4r/lyt/common.h)
// and forward-declared as a struct everywhere else (layout.h). The mismatch is
// invisible to GCC and to the console ABI, but MSVC mangles `class` and
// `struct` differently: lyt_bounding.cpp (which includes this header first)
// emitted Bounding::Bounding with a `V` (class) tag for the parameter while
// every other TU referenced it with a `U` (struct) tag -> LNK2019 on the
// Bounding constructor. Keeping the tag consistent with the definition fixes
// the link.
// =============================================================================

#include "nw4r/lyt/pane.h"

namespace nw4r {
    namespace lyt {
        struct ResBlockSet;

        class Bounding : public Pane {
        public:
            NW4R_UT_RTTI_DECL(Bounding);
            Bounding(const res::Bounding*, const ResBlockSet&);

            virtual ~Bounding();
            virtual void DrawSelf(const DrawInfo&);
        };
    };  // namespace lyt
};  // namespace nw4r
