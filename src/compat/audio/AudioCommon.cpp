// compat::audio — see AudioCommon.h.
#include "compat/audio/AudioCommon.h"

#include <mutex>
#include <unordered_map>

namespace compat::audio {

namespace {
std::mutex sMutex;
std::unordered_map<uint32_t, void*> sPtoW;
std::unordered_map<void*, uint32_t> sWtoP;
uint32_t sNext = 1; // 0 = null token
} // namespace

uint32_t storePtr(void* ptr) {
    if (ptr == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(sMutex);
    auto it = sWtoP.find(ptr);
    if (it != sWtoP.end()) {
        return it->second;
    }
    const uint32_t token = sNext++;
    sPtoW[token] = ptr;
    sWtoP[ptr] = token;
    return token;
}

void* loadPtr(uint32_t token) {
    if (token == 0) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(sMutex);
    auto it = sPtoW.find(token);
    return it == sPtoW.end() ? nullptr : it->second;
}

void clearPtrs() {
    std::lock_guard<std::mutex> lock(sMutex);
    sPtoW.clear();
    sWtoP.clear();
    sNext = 1;
}

} // namespace compat::audio
