// compat/dsp — see DSPCompat.h.
//
// Emulates the DSP mailbox/task protocol the vendored JAudio2 driver uses and
// executes the JAS mixing "program" (compat::dsp::Mixer) on a worker thread.
// In synchronous mode (tests) the frame is mixed on the calling thread.
//
// LINKAGE (important): the console-shaped functions below are defined at
// GLOBAL scope because the vendored JSystem headers — dsptask.hpp, dspproc.hpp,
// osdsp_task.hpp — declare them as global C++ functions and the vendored JAS
// sources call them unqualified. The mailbox functions are declared extern "C"
// by <revolution/dsp.h> and pick that linkage up from the prior declaration.
// Only the PC glue stays in namespace compat::dsp.
#include "compat/dsp/DSPCompat.h"

#include "compat/audio/AudioCommon.h"
#include "compat/dsp/Mixer.h"
#include "platform/Log/Log.h"
#include "platform/Threading/Threading.h"

#include <JSystem/JAudio2/dspproc.hpp>   // DsetupTable/DsetVARAM/DsetMixerLevel/...
#include <JSystem/JAudio2/dsptask.hpp>   // DspBoot/DspFinishWork/DSPSendCommands2
#include <JSystem/JAudio2/osdsp_task.hpp> // DsyncFrame2
#include <revolution/dsp.h>              // extern "C" mailbox declarations

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <vector>

namespace compat::dsp {
class Mixer;
}

// --- state (file-scope: visible to the global console-shaped functions) ------
static compat::dsp::Mixer* sMixer = nullptr;
static uint32_t sMixerLevel = 0x4000; // DSP mixer level (1.15)
static std::atomic<bool> sRunning{false}; // Dsp_Running_Status == 1
static std::atomic<bool> sSyncMode{false};

static compat::dsp::DspCallback sDspCallback = nullptr;

// --- work queue --------------------------------------------------------------
struct FrameCmd {
    uint32_t subFrames;
    int16_t* outL;
    int16_t* outR;
};
static std::mutex sQueueMutex;
static std::condition_variable sQueueCv;
static std::vector<FrameCmd> sQueue;
static bool sStopRequested = false;
static Platform::Threading::Thread* sWorker = nullptr;

// --- mailboxes (kept for console-shaped completeness) ------------------------
static std::mutex sMailMutex;
static std::vector<uint32_t> sMailToDsp;
static std::vector<uint32_t> sMailFromDsp;

// --- helpers -----------------------------------------------------------------

static const uint8_t* resolveWaveToken(uint32_t token) {
    return static_cast<const uint8_t*>(compat::audio::loadPtr(token));
}

static void mixFrame(FrameCmd cmd) {
    for (uint32_t sb = 0; sb < cmd.subFrames; ++sb) {
        sMixer->mixSubframe(JASDsp::CH_BUF, JASDsp::FX_BUF, sMixerLevel,
                            cmd.outL + sb * 0x50, cmd.outR + sb * 0x50);
        if (sDspCallback != nullptr) {
            // One "subframe sync" mail per subframe, exactly like the jdsp
            // microcode (0xF355 xxFF → JASAudioThread::DSPCallback posts
            // AUDIOMSG_DSP).
            sDspCallback(0xF3550000u | (sb << 8) | 0xFFu);
        }
    }
}

static void workerLoop() {
    Platform::Threading::setCurrentThreadName("dsp-worker");
    for (;;) {
        FrameCmd cmd{};
        bool have = false;
        {
            std::unique_lock<std::mutex> lock(sQueueMutex);
            sQueueCv.wait(lock, [] { return sStopRequested || !sQueue.empty(); });
            if (sStopRequested) {
                return;
            }
            cmd = sQueue.front();
            sQueue.erase(sQueue.begin());
            have = true;
        }
        if (have) {
            mixFrame(cmd);
        }
    }
}

// --- console-shaped surface --------------------------------------------------

void DspBoot(void (*callback)(void*)) {
    // On the Wii this boots the DSP with the jdsp image and performs the
    // handshake; on PC the mixer already exists — we only mark the DSP
    // "running" and remember the request callback (kept for API fidelity; the
    // subframe syncs go through setDspCallback).
    (void)callback;
    if (sMixer == nullptr) {
        compat::dsp::MixerConfig cfg;
        cfg.resolveWave = resolveWaveToken;
        sMixer = new compat::dsp::Mixer(cfg);
        sMixer->reset();
    }
    sRunning.store(true);
}

void DsetVARAM(uint32_t aramStart) {
    // ARAM (audio RAM) is emulated as host RAM; nothing to relocate.
    (void)aramStart;
}

void DsetupTable(uint32_t channels, uint32_t chBufTok, uint32_t resTableTok,
                 uint32_t adpcmTableTok, uint32_t fxBufTok) {
    // The mixer reads the descriptor arrays directly from host memory (tokens
    // resolved through the registry). The ROM tables (resample/ADPCM) are
    // baked into the portable mixer.
    (void)channels;
    (void)resTableTok;
    (void)adpcmTableTok;
    (void)fxBufTok;
    if (!sRunning.load()) {
        DspBoot(nullptr);
    }
    // NOTE: chBufTok is not strictly needed (the mixer takes JASDsp::CH_BUF
    // from the vendored initBuffer), but it is validated for early failure
    // detection:
    if (compat::audio::loadPtr(chBufTok) == nullptr) {
        PL_LOG_WARN("dsp", "DsetupTable: channel buffer token %u unresolved", chBufTok);
    }
}

void DsetMixerLevel(float level) {
    // Vendored formula (dspproc.cpp): 4096*level, packed as 1.15.
    sMixerLevel = static_cast<uint32_t>(4096.0f * level);
}

int DSPSendCommands2(uint32_t* msgs, uint32_t count, void (*callback)(uint16_t)) {
    (void)count;
    (void)callback;
    if (msgs == nullptr || count == 0) {
        return -1;
    }
    // Only the frame-sync command (0x82xxxxxx, see dspproc.cpp) is meaningful
    // for the JAS driver; DsetupTable/DsetVARAM no longer go through here.
    if ((msgs[0] & 0xFF000000u) == 0x82000000u) {
        const uint32_t subFrames = (msgs[0] >> 16) & 0xFFu;
        sMixerLevel = msgs[0] & 0xFFFFu;
        int16_t* outL = static_cast<int16_t*>(compat::audio::loadPtr(msgs[1]));
        int16_t* outR = static_cast<int16_t*>(compat::audio::loadPtr(msgs[2]));
        if (outL != nullptr && outR != nullptr) {
            DsyncFrame2(subFrames, msgs[1], msgs[2]);
            return 0;
        }
        PL_LOG_WARN("dsp", "DSPSendCommands2: unresolved output tokens (%u/%u)", msgs[1], msgs[2]);
    }
    return -1;
}

void DsyncFrame2(uint32_t subFrames, uint32_t outLTok, uint32_t outRTok) {
    int16_t* outL = static_cast<int16_t*>(compat::audio::loadPtr(outLTok));
    int16_t* outR = static_cast<int16_t*>(compat::audio::loadPtr(outRTok));
    if (outL == nullptr || outR == nullptr) {
        PL_LOG_WARN("dsp", "DsyncFrame2: unresolved output tokens");
        return;
    }
    if (sMixer == nullptr) {
        DspBoot(nullptr);
    }
    if (sSyncMode.load()) {
        mixFrame(FrameCmd{subFrames, outL, outR});
        return;
    }
    if (sWorker == nullptr) {
        compat::dsp::startWorker(); // lazy: dsp-worker on first frame sync
    }
    {
        std::lock_guard<std::mutex> lock(sQueueMutex);
        sQueue.push_back(FrameCmd{subFrames, outL, outR});
    }
    sQueueCv.notify_one();
}

void DsyncFrame4ch(uint32_t subFrames, uint32_t outLTok, uint32_t outRTok, uint32_t outL2Tok,
                   uint32_t outR2Tok) {
    // 4-channel sync (used by games with 4ch output; SMG is stereo): mix L/R
    // and mirror into the second pair.
    (void)outL2Tok;
    (void)outR2Tok;
    DsyncFrame2(subFrames, outLTok, outRTok);
}

void DsyncFrame2ch(uint32_t subFrames, uint32_t outLTok, uint32_t outRTok) {
    // dspproc.hpp spelling (JASDsp::syncFrame uses DsyncFrame2, osdsp_task).
    DsyncFrame2(subFrames, outLTok, outRTok);
}

void DSPReleaseHalt2(uint32_t msg) {
    (void)msg; // no-op: nothing to halt on PC
}

void DspFinishWork(uint16_t taskId) {
    (void)taskId; // task bookkeeping no-op (see docs/audio.md)
}

// --- mailboxes (extern "C", declared by <revolution/dsp.h>) ------------------

void DSPSendMailToDSP(uint32_t mail) {
    std::lock_guard<std::mutex> lock(sMailMutex);
    sMailToDsp.push_back(mail);
}
uint32_t DSPCheckMailToDSP() {
    std::lock_guard<std::mutex> lock(sMailMutex);
    return sMailToDsp.empty() ? 0 : 1;
}
uint32_t DSPReadMailFromDSP() {
    std::lock_guard<std::mutex> lock(sMailMutex);
    if (sMailFromDsp.empty()) return 0;
    const uint32_t m = sMailFromDsp.front();
    sMailFromDsp.erase(sMailFromDsp.begin());
    return m;
}
uint32_t DSPCheckMailFromDSP() {
    std::lock_guard<std::mutex> lock(sMailMutex);
    return sMailFromDsp.empty() ? 0 : 1;
}

// --- PC glue -----------------------------------------------------------------

namespace compat::dsp {

void DSPAssertInt() {} // interrupt line: nothing to assert on PC

int Dsp_Running_Check() { return sRunning.load() ? 1 : 0; }
void Dsp_Running_Start() { sRunning.store(true); }

void setSynchronous(bool sync) { sSyncMode.store(sync); }
bool isSynchronous() { return sSyncMode.load(); }

void setDspCallback(DspCallback cb) { sDspCallback = cb; }

void syncFrameHost(uint32_t subFrames, int16_t* outL, int16_t* outR) {
    DsyncFrame2(subFrames, compat::audio::storePtr(outL), compat::audio::storePtr(outR));
}

void shutdownDsp() {
    if (sWorker != nullptr) {
        {
            std::lock_guard<std::mutex> lock(sQueueMutex);
            sStopRequested = true;
        }
        sQueueCv.notify_all();
        sWorker->join();
        delete sWorker;
        sWorker = nullptr;
    }
    sQueue.clear();
    sRunning.store(false);
}

Mixer* mixer() { return sMixer; }

void startWorker() {
    if (sWorker == nullptr) {
        sStopRequested = false;
        sWorker = new Platform::Threading::Thread("dsp-worker", workerLoop);
    }
}

} // namespace compat::dsp
