// =============================================================================
// M8: JAS driver host-glue integration test (headless, virtual audio).
//
// Boots the full driver stack the way the game does at audio init:
//
//   JasAudioHost::initAudioSystem(false)
//     → JAS heap (fallback: root + solid heap, JASKernel::setupRootHeap)
//     → JAS driver thread (the JASAudioThread::run shape)
//         → JASDriver::initAI → AIInit/AIInitDMA/AIRegisterDMACallback
//         → JASDsp::boot + initBuffer (CH_BUF/FX_BUF alloc)
//         → JASDSPChannel::initAll
//         → compat/ai soft clock fires the DMA "buffer done" per frame
//         → updateDac → finishDSPFrame → compat/dsp worker mixes the frame
//         → the mixed frame lands in Platform::Audio (virtual sink)
//
// Assertions: the driver stays up, frames flow through the whole chain, and a
// clean stop joins every thread. This is the M8 "it compiles, links and the
// pipeline runs" gate in one test.
// =============================================================================

#include "tests/test_runner.h"

#include "compat/jsystem/JasAudioHost.h"
#include "platform/Audio/Audio.h"

#include <JSystem/JAudio2/JASHeapCtrl.hpp>  // JASDram
#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#include <JSystem/JKernel/JKRSolidHeap.hpp>

#include <chrono>
#include <thread>

namespace {

// The game's JAUInitializer (M9) will own the JAS heap; until then the host
// glue creates a fallback if none exists. This test does the same initializer
// dance so the driver can run — and so the test also works standalone
// (--run-test) without the jkr_heap_test having run first.
void ensureJasHeapForTest() {
    if (JASDram != nullptr) {
        return;
    }
    if (JKRHeap::sRootHeap == nullptr) {
        JKRExpHeap::createRoot(1, true);
    }
    REQUIRE(JKRHeap::sRootHeap != nullptr);
    // Solid heap must exceed the subsystem heap carved out of it (see
    // JasAudioHost.cpp, ensureJasHeap).
    JKRSolidHeap* solid = JKRSolidHeap::create(0x1000000, JKRHeap::sRootHeap, true);
    REQUIRE(solid != nullptr);
    JASKernel::setupRootHeap(solid, 0x800000 - 0x1000);
    CHECK(JASDram == solid);
}

} // namespace

TEST_CASE(jas_audio_driver_boot_and_shutdown) {
    ensureJasHeapForTest();

    REQUIRE(compat::jsystem::initAudioSystem(/*enableDevice=*/false));
    CHECK(compat::jsystem::isAudioDriverRunning());
    CHECK(Platform::Audio::isInitialized());
    CHECK(Platform::Audio::isVirtual()); // no SDL device in tests/CI

    // The ai-clock fires one DMA "buffer done" per JAS frame (~17.5 ms at
    // 32028.5 Hz); after 200 ms several frames must have been mixed and pushed
    // through the whole chain into the virtual sink.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(Platform::Audio::framesPushed() > 0);

    // Clean stop: the STOP message joins the audio thread; then the DSP
    // worker and the ai-clock are stopped and all audio subsystems shut down.
    compat::jsystem::shutdownAudioSystem();
    CHECK(!compat::jsystem::isAudioDriverRunning());
    CHECK(!Platform::Audio::isInitialized());
}
