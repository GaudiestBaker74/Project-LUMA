#pragma once
// =============================================================================
// PC_PORT PATCH of the vendored libs/nw4r/include/nw4r/lyt/group.h (see
// src/compat/patches/README.md). This shadow copy wins over the vendored
// header through the compat include path.
//
// Change vs. upstream: the non-Metrowerks LinkList typedef used offset 0
// instead of offsetof(..., mLink). That is only valid when mLink is the
// first member — but Group has a virtual destructor, so a vptr sits before mLink.
// With offset 0, LinkList::PushBack writes the prev/next pointers over the
// vptr at offset 0 and the first virtual call on the linked object crashes.
// offsetof on a polymorphic type is conditionally-supported and accepted by
// GCC/Clang/MSVC (it may warn), and yields the correct post-vptr offset.
//
// Everything else is identical to upstream.
// =============================================================================

#include "nw4r/ut/LinkList.h"
#include "nw4r/lyt/pane.h"
#include "nw4r/lyt/resources.h"
#include <cstddef>

namespace nw4r {
    namespace lyt {
        namespace detail {
            struct PaneLink {
                ut::LinkListNode mLink;
                Pane* mTarget;
            };
        };

        #ifdef __MWERKS__
        typedef ut::LinkList<detail::PaneLink, offsetof(detail::PaneLink, mLink)> PaneLinkList;
        #else
        typedef ut::LinkList<detail::PaneLink, 0>   PaneLinkList;
        #endif

        class Group {
        public:
            Group(const res::Group *, Pane *);

            virtual ~Group();

            void Init();

            const char* GetName() const {
                 return mName;
            }

            PaneLinkList& GetPaneList() { 
                return mPaneLinkList;
            }

            inline void AppendPane(Pane *);

            bool IsUserAllocated() const {
                return mbUserAllocated;
            }

            ut::LinkListNode mLink;
            PaneLinkList mPaneLinkList;
            char mName[17];
            u8 mbUserAllocated;
            u8 mPadding[2];
        };

        #ifdef __MWERKS__
        typedef ut::LinkList<Group, offsetof(Group, mLink)> GroupList;
        #else
        typedef ut::LinkList<Group, offsetof(Group, mLink)> GroupList;
        #endif

        class GroupContainer {
        public:
            GroupContainer() {

            }

            ~GroupContainer();

            void AppendGroup(Group *);

            Group* FindGroupByName(const char *);

            GroupList mGroupList;
        };
    };
};
