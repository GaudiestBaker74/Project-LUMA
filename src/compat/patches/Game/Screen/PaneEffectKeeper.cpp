// =============================================================================
// PC_PORT STUB of the vendored Game/Screen/PaneEffectKeeper.cpp (M9.5.3a).
//
// Upstream depends on the whole effect system (EffectSystemUtil, MultiEmitter,
// ParticleResourceHolder), which is far outside the M9 milestone. LayoutActor
// only needs the keeper to exist: initEffectKeeper() constructs it, kill()
// calls clear(), and MR::emitEffect()/deleteEffectAll() reach it through the
// actor. Every method here is an honest no-op/null stub so the title flow can
// run without particles; the real implementation returns with the effect
// system port (M10+). mEmitters stays default-constructed (empty array).
// =============================================================================
#include "Game/Screen/PaneEffectKeeper.hpp"
#include "Game/Screen/LayoutActor.hpp"
#include "Game/Screen/LayoutManager.hpp"

PaneEffectKeeper::PaneEffectKeeper(LayoutActor* pActor, const LayoutManager*, int, const char* pName)
    : mHost(pActor), mName(pName), mEmitters() {
}

void PaneEffectKeeper::init(const LayoutActor*, const EffectSystem*) {
}

void PaneEffectKeeper::add(const char*, const char*, const char*) {
}

MultiEmitter* PaneEffectKeeper::createEmitter(const char*) {
    return nullptr;
}

void PaneEffectKeeper::deleteEmitter(const char*) {
}

void PaneEffectKeeper::forceDeleteEmitter(const char*) {
}

void PaneEffectKeeper::deleteEmitterAll() {
}

void PaneEffectKeeper::forceDeleteEmitterAll() {
}

void PaneEffectKeeper::clear() {
}

MultiEmitter* PaneEffectKeeper::getEmitter(const char*) const {
    return nullptr;
}

void PaneEffectKeeper::changeAnim() {
}

void PaneEffectKeeper::registerEffect(MultiEmitter*, const char*) {
}

MultiEmitter* PaneEffectKeeper::find(const char*) const {
    return nullptr;
}
