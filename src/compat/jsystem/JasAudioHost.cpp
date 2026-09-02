// compat::jsystem::jas — see JasAudioHost.h.
//
// Implements the console audio-driver run-loop on a host thread. On the Wii
// this loop lives in JASAudioThread::run(); here it is reproduced 1:1 in
// plain host code (the vendored JASAudioThread.cpp needs JKRThread/OSThread,
// which land with the full JAU boot in M9). Everything downstream is the
// vendored driver: JASAiCtrl (patched), JASDSPInterface (patched), JASDSPChannel.
#include "compat/jsystem/JasAudioHost.h"

#include "compat/ai/AICompat.h"
#include "compat/audio/AudioCommon.h"
#include "compat/dsp/DSPCompat.h"
#include "platform/Audio/Audio.h"
#include "platform/Log/Log.h"
#include "platform/Threading/Threading.h"

#include <JSystem/JAudio2/JASAiCtrl.hpp>       // JASDriver::initAI/updateDac/...
#include <JSystem/JAudio2/JASAudioThread.hpp> // AUDIOMSG_*, snIntCount
#include <JSystem/JAudio2/JASChannel.hpp>      // bank-dispose queue stubs
#include <JSystem/JAudio2/JASDriverIF.hpp>     // JASDriver::updateDacCallback
#include <JSystem/JAudio2/JASDSPChannel.hpp>   // JASDSPChannel::initAll
#include <JSystem/JAudio2/JASDSPInterface.hpp> // JASDsp::boot/initBuffer
#include <JSystem/JAudio2/JASHeapCtrl.hpp>     // JASDram, JASKernel::setupRootHeap
#include <JSystem/JAudio2/JASProbe.hpp>
#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <JSystem/JKernel/JKRSolidHeap.hpp>

#include <atomic>

namespace compat::jsystem {

namespace {

Platform::Threading::MessageQueue<intptr_t>* sMsgQueue = nullptr;
Platform::Threading::Thread* sAudioThread = nullptr;
std::atomic<bool> sRunning{false};

// JAS heap ownership (the game's JAUInitializer will own it from M9; until
// then the host glue creates one so JASDriver::initAI has a JASDram).
JKRSolidHeap* sOwnedSolidHeap = nullptr;
JKRExpHeap* sOwnedRootHeap = nullptr;

// The console's DMA "buffer done" interrupt: fires on the ai-clock thread.
void dmaInterruptHost(void) {
    if (sMsgQueue != nullptr) {
        sMsgQueue->trySend(static_cast<intptr_t>(AUDIOMSG_DMA));
    }
}

// The console's DSP subframe sync interrupt: fires on the dsp-worker thread.
void dspInterruptHost(uint32_t mail) {
    (void)mail; // 0xF355xxxx sync word (all syncs are AUDIOMSG_DSP)
    if (sMsgQueue != nullptr) {
        sMsgQueue->trySend(static_cast<intptr_t>(AUDIOMSG_DSP));
    }
}

// The vendored JASDriver calls this per freshly-cooked DMA buffer (the
// console's AI would DMA it out): hand it to Platform::Audio via compat/ai.
void dspBufDone(s16* interleaved, u32 frames) {
    compat::ai::pushAudioFrame(interleaved, frames);
}

void ensureJasHeap() {
    if (JASDram != nullptr) {
        return; // the game boot already set up the JAS heap (M9)
    }
    // Mirror the game's JAU_JASInitializer shape: a root heap from the compat
    // arena + a solid heap carved out of it (the game uses the ARAM heap).
    sOwnedRootHeap = JKRExpHeap::createRoot(1, true);
    if (sOwnedRootHeap == nullptr) {
        PL_LOG_FATAL("audio", "JAS heap: createRoot failed");
        return;
    }
    // The solid heap must be strictly bigger than the subsystem heap carved
    // out of it (JASKernel::setupRootHeap creates an exp heap of `size` from
    // the solid heap) — on the Wii the ARAM heap is sized for exactly this.
    sOwnedSolidHeap = JKRSolidHeap::create(0x1000000 /* 16 MiB */,
                                           sOwnedRootHeap, true);
    if (sOwnedSolidHeap == nullptr) {
        PL_LOG_FATAL("audio", "JAS heap: JKRSolidHeap::create failed");
        return;
    }
    JASKernel::setupRootHeap(sOwnedSolidHeap, 0x800000 /* 8 MiB */
                             - 0x1000 /* header/rounding slack */);
    PL_LOG_INFO("audio", "JAS heap (host fallback): 16 MiB solid heap");
}

void audioThreadLoop() {
    Platform::Threading::setCurrentThreadName("jas-audio");

    // --- boot sequence (mirrors JASAudioThread::run) -------------------------
    JASDriver::initAI(dmaInterruptHost); // buffers + AIInit + AIStartDMA setup
    JASDsp::boot(nullptr);               // host: initializes the CPU mixer
    JASDsp::initBuffer();                // vendored: CH_BUF/FX_BUF + DsetupTable
    JASDSPChannel::initAll();            // vendored: 64 channel descriptors
    JASDriver::registDSPBufCallback(dspBufDone); // output → Platform::Audio
    compat::ai::setFrameClock(JASDriver::getFrameSamples(), JASDriver::getDacRate());
    JASDriver::startDMA();               // AIStartDMA
    compat::ai::startClock();            // per-buffer "DMA done" cadence

    PL_LOG_INFO("audio", "JAS driver up: %u samples/frame @ %.1f Hz, %u subframes",
                JASDriver::getFrameSamples(), JASDriver::getDacRate(), JASDriver::getSubFrames());

    // --- message loop (same shape as JASAudioThread::run) --------------------
    intptr_t msg = -1;
    for (;;) {
        sMsgQueue->receive(msg); // blocks
        switch (msg) {
        case AUDIOMSG_DMA:
            // One DMA buffer completed: cook the next frame (mixing is then
            // kicked by readDspBuffer/finishDSPFrame; the output is pushed via
            // dspBufDone inside updateDac).
            JASDriver::updateDac();
            JASDriver::updateDacCallback();
            break;
        case AUDIOMSG_DSP:
            JASAudioThread::snIntCount -= 1;
            if (JASAudioThread::snIntCount == 0) {
                JASProbe::stop(7);
                JASDriver::finishDSPFrame(); // re-arms + syncs the next frame
            } else {
                JASDriver::updateDSP(); // update channel states per subframe
            }
            break;
        case AUDIOMSG_STOP:
        default:
            JASDriver::stopDMA();
            compat::ai::stopClock();
            sRunning.store(false);
            return;
        }
    }
}

} // namespace

bool isAudioDriverRunning() { return sRunning.load(); }

bool initAudioSystem(bool enableDevice) {
    if (sRunning.load()) {
        return true;
    }
    compat::audio::clearPtrs();
    Platform::Audio::Config audioCfg;
    audioCfg.enable = enableDevice;
    if (!Platform::Audio::init(audioCfg)) {
        PL_LOG_FATAL("audio", "Platform::Audio init failed");
        return false;
    }
    compat::ai::init();
    compat::ai::setDmaHandler(dmaInterruptHost);
    compat::dsp::setDspCallback(dspInterruptHost);
    ensureJasHeap();
    if (JASDram == nullptr) {
        PL_LOG_FATAL("audio", "no JAS heap available");
        Platform::Audio::shutdown();
        return false;
    }

    sMsgQueue = new Platform::Threading::MessageQueue<intptr_t>(0x10);
    sRunning.store(true);
    sAudioThread = new Platform::Threading::Thread("jas-audio", audioThreadLoop);
    return true;
}

void shutdownAudioSystem() {
    if (sAudioThread != nullptr) {
        sMsgQueue->send(static_cast<intptr_t>(AUDIOMSG_STOP));
        sAudioThread->join();
        delete sAudioThread;
        sAudioThread = nullptr;
    }
    delete sMsgQueue;
    sMsgQueue = nullptr;
    compat::dsp::shutdownDsp();
    compat::ai::shutdown();
    Platform::Audio::shutdown();
    compat::audio::clearPtrs();
    if (sOwnedSolidHeap != nullptr) {
        sOwnedSolidHeap->freeAll(); // release the JAS buffers
        sOwnedSolidHeap = nullptr;
    }
    if (sOwnedRootHeap != nullptr) {
        sOwnedRootHeap->freeAll();
        sOwnedRootHeap = nullptr;
    }
}

} // namespace compat::jsystem

// --- vendored-static glue -----------------------------------------------------
// JASAudioThread::run()'s subframe counter (declared in the vendored header;
// the vendored .cpp that defines it is not compiled on the host).
volatile int JASAudioThread::snIntCount = 0;

// JASChannel bank-dispose queue: the vendored JASChannel.cpp is not compiled
// (it needs the OS message-queue/OSThread layer). These two are the only
// JASChannel symbols the driver calls; both are no-ops (the queue is empty:
// nothing disposes banks through it yet). TODO(PC_PORT): restore real
// semantics when JASChannel.cpp lands (M8.5).
void JASChannel::initBankDisposeMsgQueue() {}
void JASChannel::receiveBankDisposeMsg() {}

// ARAM base address (revolution/aralt.h): ARAM is emulated as host RAM, so the
// emulated audio heap starts at offset 0. Only the symbol matters for
// JASKernel::setupAramHeap/ARGetBaseAddress callers.
extern "C" uint32_t ARGetBaseAddress(void) { return 0; }
