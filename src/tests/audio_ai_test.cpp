// =============================================================================
// M8: compat/ai — AI registers/DMA emulation + the JAS-frame resampler, in
// Platform::Audio virtual mode (no SDL device needed — headless).
//
// The AI layer is what JASAiCtrl (vendored) drives: AIInit/AIInitDMA/
// AIStartDMA/AIStopDMA + the DMA "buffer done" callback cadence. The PC glue
// resamples the DSP output (32028.5 Hz) down to the Platform::Audio input rate
// (32000 Hz) and pushes it. This test runs the pipeline manually (pumpOnce)
// so results are deterministic.
// =============================================================================

#include "tests/test_runner.h"

#include "compat/ai/AICompat.h"
#include "platform/Audio/Audio.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

uint32_t sDmaCallbacks = 0;
void countingDmaHandler(void) { ++sDmaCallbacks; }

} // namespace

TEST_CASE(ai_dma_registers_and_callback) {
    // Virtual mode: config.enable = false skips SDL entirely.
    Platform::Audio::Config cfg;
    cfg.enable = false;
    REQUIRE(Platform::Audio::init(cfg));
    CHECK(Platform::Audio::isInitialized());
    CHECK(Platform::Audio::isVirtual());

    compat::ai::init();
    // Console-style registration (JASAiCtrl::initAI calls AIRegisterDMACallback).
    AIRegisterDMACallback(countingDmaHandler);

    // Register surface (JASAiCtrl::initAI calls these).
    AIInitDMA(0x12345678u, 1120);
    CHECK(AIGetDMAStartAddr() == 0x12345678u);
    CHECK(AIGetDMALength() == 1120u);
    AISetDSPSampleRate(0);
    CHECK(!compat::ai::isStreaming());

    // DMA callback fires only while streaming.
    AIStartDMA();
    CHECK(compat::ai::isStreaming());
    compat::ai::pumpOnce();
    CHECK(sDmaCallbacks == 1);
    AIStopDMA();
    compat::ai::pumpOnce();
    CHECK(sDmaCallbacks == 1); // stopped: no interrupt

    CHECK(AIRegisterDMACallback(nullptr) != nullptr); // previous handler

    compat::ai::shutdown();
    Platform::Audio::shutdown();
}

TEST_CASE(ai_resample_and_push) {
    Platform::Audio::Config cfg;
    cfg.enable = false;
    REQUIRE(Platform::Audio::init(cfg));

    compat::ai::init();
    // The JAS driver geometry: 560 samples per frame @ 32028.5 Hz.
    compat::ai::setFrameClock(560, 32028.5f);

    // A 440 Hz sine, interleaved stereo (one JAS DMA buffer).
    std::vector<int16_t> frame(560 * 2);
    for (uint32_t i = 0; i < 560; ++i) {
        const float t = static_cast<float>(i) / 32000.0f;
        const int16_t v = static_cast<int16_t>(std::sin(2.0f * 3.14159265f * 440.0f * t) * 20000.0f);
        frame[static_cast<size_t>(i) * 2 + 0] = v;
        frame[static_cast<size_t>(i) * 2 + 1] = v;
    }

    const uint64_t pushedBefore = Platform::Audio::framesPushed();
    compat::ai::pushAudioFrame(frame.data(), 560);
    CHECK(Platform::Audio::framesPushed() > pushedBefore);
    // 560 * 32000/32028.47 ≈ 559.5 → 559 frames (queuedFrames() = frames,
    // each 2 ch; pull() returns s16 samples, i.e. 2 * frames).
    const int queued = Platform::Audio::queuedFrames();
    CHECK(queued >= 558 && queued <= 561);

    // The device side can now pull the frames out of the ring.
    std::vector<int16_t> out(4096);
    const int n = Platform::Audio::pull(out.data(), 4096);
    CHECK(n == queued * 2);
    CHECK(out[32] != 0); // the sine is audible, not silence (sample 0 = sin 0)

    // A second frame continues the stream (phase accumulator carries over).
    compat::ai::pushAudioFrame(frame.data(), 560);
    CHECK(Platform::Audio::queuedFrames() > 0);

    compat::ai::shutdown();
    Platform::Audio::shutdown();
}
