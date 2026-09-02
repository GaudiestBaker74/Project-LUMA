// =============================================================================
// PC_PORT PATCH of the vendored Game/Scene/SceneFactory.cpp (see
// patches/README.md).
//
// Change: the upstream decompiled loop breaks on the first non-matching entry
// (`if (!isEqualName) break;`), which makes MR::createScene always return a
// GameScene. The console code breaks on the match; restored here so
// MR::createScene("Logo") really creates the LogoScene.
// =============================================================================
#include "Game/Scene/SceneFactory.hpp"
#include "Game/Scene/GameScene.hpp"
#include "Game/Scene/IntermissionScene.hpp"
#include "Game/Scene/LogoScene.hpp"
#include "platform/Log/Log.h"
#include <cstdio>

namespace {
    struct Name2CreateFunc {
        /* 0x0 */ const char* mName;
        /* 0x4 */ Scene* (*mCreateFunc)();
    };

    template < typename T >
    Scene* createScene() {
        return new T();
    }

    const Name2CreateFunc cCreateTable[] = {
        {"Game", createScene< GameScene >},
        {"Intermission", createScene< IntermissionScene >},
        {"Logo", createScene< LogoScene >},
    };
};  // namespace

namespace MR {
    Scene* createScene(const char* pName) {
        const ::Name2CreateFunc* pBegin = &::cCreateTable[0];
        const ::Name2CreateFunc* pEnd = &::cCreateTable[ARRAY_SIZE(::cCreateTable)];
        const ::Name2CreateFunc* pIter;

        for (pIter = pBegin; pIter != pEnd; pIter++) {
            bool isEqualName = strcmp(pIter->mName, pName) == 0;

            if (isEqualName) {
                break;
            }
        }

        if (pIter == pEnd) {
            return nullptr;
        }

        // PC_PORT (M9.4): boot-progress marker — scene creation happens on an
        // async worker (GameSystemSceneController::initializeScene), so this
        // log line is the confirmation that the scene machine really created
        // the requested scene.
        PL_LOG_INFO("boot", "createScene('%s')", pName);

        return (*pIter->mCreateFunc)();
    }
};  // namespace MR
