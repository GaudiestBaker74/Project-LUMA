// compat/ai — see AICompat.h.
#include "compat/ai/AICompat.h"

#include "platform/Audio/Audio.h"
#include "platform/Threading/Threading.h"
#include "platform/Timing/Timing.h"
#include "platform/platform.h"

#include <revolution/ai.h> // console API types (AIDCallback)

#include <atomic>
#include <cmath>
#include <mutex>

namespace compat::ai {

namespace {

std::mutex sMutex;
bool sInitialized = false;

// AI register state (what the emulation needs; not all registers exist).
bool sStreaming = false;      // AIStartDMA'd
uint32_t sDmaStartAddr = 0;   // token, last AIInitDMA
uint32_t sDmaLength = 0;      // bytes, last AIInitDMA
uint32_t sDspRateSel = 0;     // AISetDSPSampleRate argument
void* sDmaCallback = nullptr; // AIRegisterDMACallback

// Frame clock (JAS frame geometry, set by the driver glue).
std::atomic<uint32_t> sFrameSamples{560};
std::atomic<float> sDacRate{32028.5f};
std::atomic<bool> sClockRunning{false};
std::atomic<bool> sStopRequested{false};
DmaCallback sDmaHandler = nullptr;
bool sClockPresent = false;
Platform::Threading::Thread* sClockThread = nullptr;

// Resampler state: DSP output rate (sDacRate) → Platform::Audio input rate.
float sSrcPhase = 0.0f;
int32_t sSrcLast = 0;
bool sSrcHasLast = false;

// Device-tick accounting.
std::atomic<int> sLastDeviceTick{0};

void aiClockLoop() {
    Platform::Threading::setCurrentThreadName("ai-clock");
    const double periodSeconds = static_cast<double>(sFrameSamples.load()) / sDacRate.load();
    const uint64_t periodNs = static_cast<uint64_t>(periodSeconds * 1e9);
    uint64_t next = Platform::Timing::nanosecondsSince(Platform::Timing::now());
    while (!sStopRequested.load()) {
        next += periodNs;
        const uint64_t now = Platform::Timing::nanosecondsSince(Platform::Timing::now());
        if (next > now) {
            Platform::Timing::sleepMicroseconds((next - now) / 1000);
        } else {
            next = now; // fell behind: resync (no burst)
        }
        if (sStreaming) {
            // Console order: the registered DMA callback (JASAiCtrl's
            // AIRegisterDMACallback) wins; the host glue handler is the
            // fallback for direct test wiring.
            if (sDmaCallback != nullptr) {
                reinterpret_cast<AIDCallback>(sDmaCallback)();
            } else if (sDmaHandler != nullptr) {
                sDmaHandler();
            }
        }
    }
}

} // namespace

bool isInitialized() { return sInitialized; }
bool isStreaming() { return sStreaming; }

void init() {
    std::lock_guard<std::mutex> lock(sMutex);
    if (sInitialized) {
        return;
    }
    sInitialized = true;
    sStreaming = false;
    sSrcHasLast = false;
    sSrcPhase = 0.0f;
}

void shutdown() {
    stopClock();
    std::lock_guard<std::mutex> lock(sMutex);
    sInitialized = false;
    sStreaming = false;
    sDmaHandler = nullptr;
}

void setFrameClock(uint32_t frameSamples, float dacRate) {
    sFrameSamples.store(frameSamples ? frameSamples : 560);
    sDacRate.store(dacRate > 0.0f ? dacRate : 32028.5f);
}

void startClock() {
    if (sClockPresent) {
        return;
    }
    sStopRequested.store(false);
    sClockPresent = true;
    sClockThread = new Platform::Threading::Thread("ai-clock", aiClockLoop);
}

void stopClock() {
    if (!sClockPresent) {
        return;
    }
    sStopRequested.store(true);
    sClockThread->join();
    delete sClockThread;
    sClockThread = nullptr;
    sClockPresent = false;
}

void setDmaHandler(DmaCallback cb) { sDmaHandler = cb; }

void pumpOnce() {
    if (!sStreaming) {
        return; // the console raises no interrupts while the DMA is stopped
    }
    if (sDmaCallback != nullptr) {
        reinterpret_cast<AIDCallback>(sDmaCallback)();
    } else if (sDmaHandler != nullptr) {
        sDmaHandler();
    }
}

void pushAudioFrame(const int16_t* interleaved, uint32_t frames) {
    if (!sInitialized || interleaved == nullptr || frames == 0) {
        return;
    }
    // Linear resample from the DSP output rate to the Platform::Audio input
    // rate (32000 Hz). Output length scales by the rate ratio; the phase
    // accumulator makes the average ratio exact (no long-term drift).
    const float outRate = 32000.0f; // Platform::Audio input spec
    const float ratio = outRate / sDacRate.load();
    const uint32_t outFrames = static_cast<uint32_t>(static_cast<float>(frames) * ratio);

    static thread_local int16_t scratch[4096];
    if (outFrames * 2 > static_cast<uint32_t>(sizeof(scratch) / sizeof(scratch[0]))) {
        PL_LOG_WARN("ai", "frame too large (%u) for the resampler scratch — dropped", outFrames);
        return;
    }
    for (uint32_t f = 0; f < outFrames; ++f) {
        const float pos = sSrcPhase;
        const uint32_t i = static_cast<uint32_t>(pos);
        const float frac = pos - static_cast<float>(i);
        const int32_t i0 = static_cast<int32_t>(i);
        const int32_t i1 = i0 + 1 >= static_cast<int32_t>(frames) ? i0 : i0 + 1;
        for (int c = 0; c < 2; ++c) {
            const float a = static_cast<float>(interleaved[i0 * 2 + c]);
            float b = a;
            if (i1 != i0) {
                b = static_cast<float>(interleaved[i1 * 2 + c]);
            }
            scratch[f * 2 + c] = static_cast<int16_t>(a + (b - a) * frac);
        }
        sSrcPhase += 1.0f / ratio;
        if (sSrcPhase >= static_cast<float>(frames) - 1.0f) {
            sSrcPhase -= static_cast<float>(frames) - 1.0f;
        }
    }
    Platform::Audio::push(scratch, static_cast<int>(outFrames));
}

void onDeviceTick(void*, int framesRequested) {
    sLastDeviceTick.store(framesRequested, std::memory_order_relaxed);
}

} // namespace compat::ai

// --- console API (C linkage) -------------------------------------------------
// Defined at global scope (unmangled C symbols, as the game expects). The
// using-directive exposes compat::ai (including its anonymous namespace) to
// the definitions below.
using namespace compat::ai;

extern "C" {

void AIInit(uint8_t* dspAram) {
    (void)dspAram;
    compat::ai::init();
    // The registered DMA callback survives re-init, like re-running the SDK.
}

void AIInitDMA(uint32_t addr, uint32_t len) {
    sDmaStartAddr = addr; // token (see docs/audio.md §"32-bit pointer ABI")
    sDmaLength = len;
}

void AIStartDMA(void) { sStreaming = true; }

void AIStopDMA(void) { sStreaming = false; }

uint32_t AIGetDMAStartAddr(void) { return sDmaStartAddr; }

uint32_t AIGetDMALength(void) { return sDmaLength; }

void AISetDSPSampleRate(uint32_t rate) { sDspRateSel = rate; }

AIDCallback AIRegisterDMACallback(AIDCallback callback) {
    AIDCallback previous = reinterpret_cast<AIDCallback>(sDmaCallback);
    sDmaCallback = reinterpret_cast<void*>(callback);
    return previous;
}

} // extern "C"

