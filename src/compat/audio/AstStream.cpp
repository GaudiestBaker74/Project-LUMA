// compat::audio::AstStream — see AstStream.h.
//
// PC_PORT (M9.5.4 v7). Threading model: startStream() opens the file on the
// caller's thread (header check only), then a decode thread reads block by
// block, converts to interleaved s16 stereo and pushes into Platform::Audio.
// Platform::Audio::push is non-blocking and drops what does not fit, so the
// thread paces itself on queuedFrames(): it keeps ~one ring of audio queued
// and sleeps otherwise. Fades are applied per pushed chunk (60 Hz frame units
// as the game passes them: stopStageBGM(75) = 1.25 s).
#include "compat/audio/AstStream.h"

#include "platform/Audio/Audio.h"
#include "platform/Filesystem/Filesystem.h"
#include "platform/Log/Log.h"
#include "platform/Threading/Threading.h"
#include "platform/Timing/Timing.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace compat::audio {

namespace {

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t rd32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

constexpr size_t kHeaderSize = 0x40;
constexpr size_t kBlockHeaderSize = 0x20;
constexpr size_t kAfcFrameBytes = 9;
constexpr size_t kAfcFrameSamples = 16;

// AFC coefficient table (afc_decoder.c), Q11.
const int16_t kAfcCoef[16][2] = {
    {0, 0},        {2048, 0},     {0, 2048},     {1024, 1024},  {4096, -2048}, {3584, -1536},
    {3072, -1024}, {4608, -2560}, {4200, -2248}, {4800, -2300}, {5120, -3072}, {2048, -2048},
    {1024, -1024}, {-1024, 1024}, {-1024, 0},    {-2048, 0},
};

int16_t clamp16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return static_cast<int16_t>(v);
}

// Decodes one BLCK payload (all channels of the block) into interleaved
// stereo appended to `out`. `hist` carries the AFC history per channel.
// Returns the number of frames (per-channel samples) produced.
size_t decodeBlock(const AstInfo& info, const uint8_t* payload, size_t bytesPerChannel,
                   std::vector<int16_t>& out, int16_t hist[][2]) {
    size_t frames = 0;

    if (info.codec == 1) {
        frames = bytesPerChannel / 2;
    } else {
        frames = (bytesPerChannel / kAfcFrameBytes) * kAfcFrameSamples;
    }
    if (frames == 0) {
        return 0;
    }

    const size_t base = out.size();
    out.resize(base + frames * 2);

    // Decode up to two channels; mono is duplicated into both outputs.
    const int useChannels = info.channels >= 2 ? 2 : 1;
    std::vector<int16_t> scratch(frames);
    for (int ch = 0; ch < useChannels; ++ch) {
        const uint8_t* src = payload + bytesPerChannel * static_cast<size_t>(ch);
        if (info.codec == 1) {
            for (size_t i = 0; i < frames; ++i) {
                scratch[i] = static_cast<int16_t>(rd16(src + i * 2));
            }
        } else {
            decodeAfcFrames(src, bytesPerChannel / kAfcFrameBytes, scratch.data(), hist[ch]);
        }
        for (size_t i = 0; i < frames; ++i) {
            if (ch == 0) {
                out[base + i * 2 + 0] = scratch[i];
            }
            if (ch == 1 || useChannels == 1) {
                out[base + i * 2 + 1] = scratch[i];
            }
        }
    }
    return frames;
}

// Walks the BLCK chain: calls fn(payload, bytesPerChannel) per block. Stops
// at EOF or the first malformed block. Returns the number of blocks visited.
template <typename Fn>
size_t forEachBlock(const uint8_t* data, size_t size, const AstInfo& info, size_t firstBlockOffset, Fn&& fn) {
    size_t off = firstBlockOffset;
    size_t blocks = 0;
    while (off + kBlockHeaderSize <= size) {
        if (std::memcmp(data + off, "BLCK", 4) != 0) {
            break;
        }
        const size_t bytesPerChannel = rd32(data + off + 4);
        const size_t payloadBytes = bytesPerChannel * info.channels;
        if (bytesPerChannel == 0 || off + kBlockHeaderSize + payloadBytes > size) {
            break;
        }
        if (!fn(data + off + kBlockHeaderSize, bytesPerChannel)) {
            break;
        }
        off += kBlockHeaderSize + payloadBytes;
        ++blocks;
    }
    return blocks;
}

// --- player state -------------------------------------------------------------

struct Player {
    std::mutex mutex;                          // guards start/stop/join
    Platform::Threading::Thread* thread = nullptr;
    std::atomic<bool> quit{false};
    std::atomic<bool> playing{false};
    std::atomic<float> volume{1.0f};           // music bus
    std::atomic<int> fadeOutFrames{-1};        // >= 0: fade-out requested
    std::string path;
    std::string pathSnapshot;                  // for currentStreamPath() (stable storage)
};

Player& player() {
    static Player p;
    return p;
}

// Per-stream label → file table (lumasworkshop List_of_Music, SMG1). Labels
// missing here resolve to "/AudioRes/Stream/<label>.ast" (never exists on the
// disc, but keeps the lookup total; the caller logs the miss).
struct LabelEntry {
    const char* label;
    const char* file;
};

const LabelEntry kLabelTable[] = {
    // --- PC_PORT (M10.1): the MBGM_* stage-BGM labels ------------------------
    // The game does NOT start streams by their STM_* file label: it calls
    // MR::startStageBGM("MBGM_FILE_SELECT", …) / playStageBGM("MBGM_…"), and on
    // the console the SMR sound archive resolves that music id to the stream
    // (and to its "near/far" variants used by setStageBGMState). The host has
    // no SMR archive, so the music ids the game actually uses are listed here
    // and mapped to the same stream files as their STM_* twins — without this
    // every MBGM_* request fell through to "/AudioRes/Stream/MBGM_X.ast",
    // which does not exist, i.e. the screen ran silent (the M10.1 bug: the
    // FileSelect BGM never played while the title's STM_TITLE stream did).
    {"MBGM_FILE_SELECT", "SMG_fileselect_strm"},
    {"MBGM_STAR_EXIST", "SMG_ev_starchance_strm"},
    {"MBGM_STAR_EXIST_2", "SMG_ev_starchance02_strm"},
    {"MBGM_RACE_01", "SMG_ev_race_strm"},
    {"MBGM_RACE_02", "SMG_ev_race02_multi"},
    {"MBGM_GALAXY_05", "SMG_galaxy05_strm"},
    {"MBGM_GALAXY_15_HURRY", "SMG_galaxy15_hurry_strm"},
    {"MBGM_GALAXY_02_HURRY", "SMG_galaxy02_hurry_strm"},
    {"MBGM_GALAXY_02_CHASE", "SMG_ev_rabbit_strm"},
    {"MBGM_GALAXY_24", "SMG_galaxy24_multi"},
    {"MBGM_GALAXY_INTER", "SMG_galaxy_inter_strm"},
    {"MBGM_BOSS_01_A", "SMG_boss01a_strm"},
    {"MBGM_BOSS_01_B", "SMG_boss01b_strm"},
    {"MBGM_BOSS_02_A", "SMG_boss02a_strm"},
    {"MBGM_BOSS_02_B", "SMG_boss02a_strm"},
    {"MBGM_BOSS_03_A", "SMG_boss03a_strm"},
    {"MBGM_BOSS_03_B", "SMG_boss03b_strm"},
    {"MBGM_BOSS_04", "SMG_boss04_strm"},
    {"MBGM_BOSS_05_A", "SMG_boss05a_strm"},
    {"MBGM_BOSS_05_B", "SMG_boss05b_strm"},
    {"MBGM_BOSS_06_A", "SMG_boss06a_strm"},
    {"MBGM_BOSS_06_B", "SMG_boss06b_strm"},
    {"MBGM_BOSS_09_A", "SMG_boss09a_strm"},
    {"MBGM_BOSS_09_B", "SMG_boss09b_strm"},
    {"MBGM_BOSS_KOOPA_FINAL", "SMG_boss09b_strm"},
    {"MBGM_ASTRO_DOME_2", "SMG_astrodome02_strm"},
    // STM_* are the ids the game uses for the *streamed* sequences
    // (startStageBGM("STM_PROLOGUE_01")-style calls and the STM ids in
    // Game/System/GameSequence*).
    {"STM_TITLE", "SMG_title_strm"},
    {"STM_FILE_SELECT", "SMG_fileselect_strm"},
    {"STM_STAFF_ROLL", "SMG_staffroll_strm"},
    {"STM_LIBRARY", "SMG_astrodome03_strm"},
    {"STM_ASTRO_OUT", "SMG_astroout01_strm"},
    {"STM_ASTRO_OUT_2", "SMG_astroout02_strm"},
    {"STM_ASTRO_OUT_3", "SMG_astroout03_strm"},
    {"STM_ASTRO_DOME", "SMG_astrodome_multi"},
    {"STM_ASTRO_DOME_2", "SMG_astrodome02_strm"},
    {"STM_ASTRO_DOME_LOFT", "SMG_astrodome04_multi"},
    {"STM_GALAXY_01", "SMG_galaxy01_strm"},
    {"STM_GALAXY_02", "SMG_galaxy02_strm"},
    {"STM_GALAXY_03", "SMG_galaxy03_multi"},
    {"STM_GALAXY_04", "SMG_galaxy04_multi"},
    {"STM_GALAXY_05", "SMG_galaxy05_strm"},
    {"STM_GALAXY_06", "SMG_galaxy06_strm"},
    {"STM_GALAXY_07", "SMG_galaxy07_strm"},
    {"STM_GALAXY_08", "SMG_galaxy08_strm"},
    {"STM_GALAXY_09", "SMG_galaxy09_strm"},
    {"STM_GALAXY_10", "SMG_galaxy10_strm"},
    {"STM_GALAXY_11", "SMG_galaxy11_multi"},
    {"STM_GALAXY_12", "SMG_galaxy12_strm"},
    {"STM_GALAXY_13", "SMG_galaxy13_strm"},
    {"STM_GALAXY_14", "SMG_galaxy14_strm"},
    {"STM_GALAXY_15", "SMG_galaxy15_strm"},
    {"STM_GALAXY_16", "SMG_galaxy16_multi"},
    {"STM_GALAXY_17", "SMG_galaxy17_strm"},
    {"STM_GALAXY_18", "SMG_galaxy18_strm"},
    {"STM_GALAXY_19", "SMG_galaxy19_strm"},
    {"STM_GALAXY_20", "SMG_galaxy20_strm"},
    {"STM_GALAXY_21", "SMG_galaxy21_strm"},
    {"STM_GALAXY_22", "SMG_galaxy22_strm"},
    {"STM_GALAXY_23", "SMG_galaxy23_strm"},
    {"STM_GALAXY_24", "SMG_galaxy24_multi"},
    {"STM_GALAXY_25", "SMG_galaxy25_strm"},
    {"STM_GALAXY_26", "SMG_galaxy26_strm"},
    {"STM_GALAXY_27", "SMG_galaxy27_strm"},
    {"STM_GALAXY_28", "SMG_galaxy28_strm"},
    {"STM_GALAXY_10_HURRY", "SMG_galaxy10_hurry_strm"},
    {"STM_GALAXY_15_HURRY", "SMG_galaxy15_hurry_strm"},
    {"STM_GALAXY_02_HURRY", "SMG_galaxy02_hurry_strm"},
    {"STM_GALAXY_INTER", "SMG_galaxy_inter_strm"},
    {"STM_GALAXY_01_TOMB", "SMG_galaxy01_tomb_strm"},
    {"STM_GALAXY_02_CHASE", "SMG_ev_rabbit_strm"},
    {"STM_KINOPIO_TANKEN", "SMG_ev_kinotan_strm"},
    {"STM_KINOPIO_TANKEN_B", "SMG_ev_kinotan_strm"},
    {"STM_RACE_01", "SMG_ev_race_strm"},
    {"STM_RACE_02", "SMG_ev_race02_multi"},
    {"STM_MEET_KOOPA", "SMG_ev_kuppabt01_strm"},
    {"STM_STAR_EXIST", "SMG_ev_starchance_strm"},
    {"STM_STAR_EXIST_2", "SMG_ev_starchance02_strm"},
    {"STM_BOSS_01a", "SMG_boss01a_strm"},
    {"STM_BOSS_01b", "SMG_boss01b_strm"},
    {"STM_BOSS_02a", "SMG_boss02a_strm"},
    {"STM_BOSS_02b", "SMG_boss02b_strm"},
    {"STM_BOSS_03a", "SMG_boss03a_strm"},
    {"STM_BOSS_03b", "SMG_boss03b_strm"},
    {"STM_BOSS_04", "SMG_boss04_strm"},
    {"STM_BOSS_05a", "SMG_boss05a_strm"},
    {"STM_BOSS_05b", "SMG_boss05b_strm"},
    {"STM_BOSS_06a", "SMG_boss06a_strm"},
    {"STM_BOSS_06b", "SMG_boss06b_strm"},
    {"STM_BOSS_KOOPA", "SMG_boss07_multi"},
    {"STM_BOSS_09_A", "SMG_boss09a_multi"},
    {"STM_BOSS_09_B", "SMG_boss09b_multi"},
    {"STM_BOSS_MECHA_KOOPA", "SMG_boss08_strm"},
    {"STM_BOSS_KOOPA_FINAL", "SMG_boss10_strm"},
    {"STM_PROLOGUE_01", "SMG_ev_prolo01_strm"},
    {"STM_PROLOGUE_01_B", "SMG_ev_prolo01_b_strm"},
    {"STM_PROLOGUE_02", "SMG_ev_prolo02_strm"},
    {"STM_PROLOGUE_03", "SMG_ev_prolo03_strm"},
    {"STM_PROLOGUE_04", "SMG_ev_prolo04_strm"},
    {"STM_PROLOGUE_05", "SMG_ev_prolo05_strm"},
    {"STM_EPILOGUE_B", "SMG_ev_epilogue_b_strm"},
    {"STM_FIRST_ASTRO", "SMG_first_astro_strm"},
    {"STM_SECOND_ASTRO", "SMG_second_astro_strm"},
};

int stringCaseCompare(const char* a, const char* b) {
    for (;; ++a, ++b) {
        const int ca = (*a >= 'A' && *a <= 'Z') ? *a - 'A' + 'a' : *a;
        const int cb = (*b >= 'A' && *b <= 'Z') ? *b - 'A' + 'a' : *b;
        if (ca != cb) return ca - cb;
        if (ca == 0) return 0;
    }
}

void applyGain(int16_t* samples, size_t count, float gain) {
    if (gain >= 0.999f) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        samples[i] = static_cast<int16_t>(samples[i] * gain);
    }
}

// Linear resampler for the (theoretical) case of a stream whose rate differs
// from the Platform::Audio input rate. SMG's discs ship 32 kHz streams and the
// boot opens the device at 32 kHz, so this is normally a pass-through.
struct LinearResampler {
    double ratio = 1.0;   // input frames per output frame
    double pos = 0.0;     // fractional read position within the pending input
    int16_t last[2] = {0, 0};
    bool primed = false;

    void process(const int16_t* in, size_t inFrames, std::vector<int16_t>& out) {
        out.clear();
        if (inFrames == 0) {
            return;
        }
        // Virtual input: [last, in[0..inFrames-1]] so interpolation can cross
        // chunk boundaries. Index 0 = last, index k = in[k-1].
        const double totalIn = static_cast<double>(inFrames) + (primed ? 1.0 : 0.0);
        if (!primed) {
            last[0] = in[0];
            last[1] = in[1];
            primed = true;
            pos = 0.0;
        }
        auto sampleAt = [&](size_t idx, int ch) -> int32_t {
            if (idx == 0) return last[ch];
            return in[(idx - 1) * 2 + ch];
        };
        while (pos + 1.0 < totalIn) {
            const size_t i0 = static_cast<size_t>(pos);
            const double t = pos - static_cast<double>(i0);
            for (int ch = 0; ch < 2; ++ch) {
                const int32_t a = sampleAt(i0, ch);
                const int32_t b = sampleAt(i0 + 1, ch);
                out.push_back(static_cast<int16_t>(a + (b - a) * t));
            }
            pos += ratio;
        }
        // Keep the last input frame and rebase the position onto it.
        last[0] = in[(inFrames - 1) * 2 + 0];
        last[1] = in[(inFrames - 1) * 2 + 1];
        pos -= (totalIn - 1.0);
        if (pos < 0.0) pos = 0.0;
    }
};

// The decode thread body.
void decodeThreadMain(std::vector<uint8_t> file, AstInfo info, bool loop, int fadeInFrames) {
    Player& p = player();
    const uint8_t* data = file.data();
    const size_t size = file.size();

    // Pre-scan the block chain so loop seeks can jump by sample position.
    struct BlockRef {
        size_t offset;         // of the BLCK header
        size_t bytesPerChannel;
        uint32_t firstSample;  // per-channel sample index of the block start
        uint32_t frames;
    };
    std::vector<BlockRef> blocks;
    {
        uint32_t sample = 0;
        size_t off = kHeaderSize;
        forEachBlock(data, size, info, kHeaderSize, [&](const uint8_t* payload, size_t bytesPerChannel) {
            BlockRef ref;
            ref.offset = static_cast<size_t>(payload - data) - kBlockHeaderSize;
            ref.bytesPerChannel = bytesPerChannel;
            ref.firstSample = sample;
            ref.frames = info.codec == 1 ? static_cast<uint32_t>(bytesPerChannel / 2)
                                         : static_cast<uint32_t>((bytesPerChannel / kAfcFrameBytes) * kAfcFrameSamples);
            sample += ref.frames;
            blocks.push_back(ref);
            (void)off;
            return true;
        });
        if (info.numSamples == 0 || info.numSamples > sample) {
            info.numSamples = sample;
        }
    }
    if (blocks.empty()) {
        PL_LOG_WARN("game.audio", "AST '%s': no BLCK blocks — nothing to play", p.path.c_str());
        p.playing.store(false);
        return;
    }

    const bool useLoop = loop && info.looped && info.loopEnd > info.loopStart && info.loopStart < info.numSamples;
    const uint32_t endSample = useLoop ? std::min(info.loopEnd, info.numSamples) : info.numSamples;

    // Fade bookkeeping in output frames (the game gives 60 Hz frame counts).
    const double framesPerGameFrame = info.sampleRate / 60.0;
    const uint64_t fadeInTotal = static_cast<uint64_t>(std::max(0, fadeInFrames) * framesPerGameFrame);
    uint64_t played = 0;              // frames pushed since start (for fade-in)
    uint64_t fadeOutTotal = 0;        // > 0 once a fade-out is armed
    uint64_t fadeOutDone = 0;

    // Pacing: Platform::Audio::push drops what does not fit, so pushes go out
    // in small pieces and wait for room (the ring is ~250 ms at boot).
    const int capacity = std::max(1024, Platform::Audio::capacityFrames());
    const int pieceFrames = 512;
    auto pushPaced = [&](const int16_t* frames, size_t count) {
        size_t done = 0;
        while (done < count && !p.quit.load(std::memory_order_relaxed)) {
            const int n = static_cast< int >(std::min< size_t >(pieceFrames, count - done));
            while (!p.quit.load(std::memory_order_relaxed) && Platform::Audio::queuedFrames() + n > capacity - 64) {
                Platform::Timing::sleepSeconds(0.004);
            }
            if (p.quit.load(std::memory_order_relaxed)) {
                return;
            }
            Platform::Audio::push(frames + done * 2, n);
            done += static_cast< size_t >(n);
        }
    };

    std::vector<int16_t> pcm;
    std::vector<int16_t> resampled;
    LinearResampler resampler;
    const int outputRate = Platform::Audio::inputFreq() > 0 ? Platform::Audio::inputFreq() : static_cast<int>(info.sampleRate);
    resampler.ratio = static_cast<double>(info.sampleRate) / static_cast<double>(outputRate);
    const bool needResample = outputRate != static_cast<int>(info.sampleRate);
    if (needResample) {
        PL_LOG_INFO("game.audio", "AST '%s': resampling %u Hz -> %d Hz (linear)", p.path.c_str(), info.sampleRate,
                    outputRate);
    }
    int16_t hist[2][2] = {{0, 0}, {0, 0}};
    size_t blockIdx = 0;
    uint32_t blockSkip = 0;  // frames to skip at the start of the current block (loop seek)
    bool finished = false;

    PL_LOG_INFO("game.audio", "AST '%s': %u Hz, %u ch, %s, %u samples, loop %s [%u..%u], %zu blocks",
                p.path.c_str(), info.sampleRate, info.channels, info.codec == 1 ? "PCM16" : "AFC",
                info.numSamples, useLoop ? "yes" : "no", info.loopStart, info.loopEnd, blocks.size());

    while (!p.quit.load(std::memory_order_relaxed) && !finished) {
        // Arm the fade-out when the game asks for it.
        const int requestedFadeOut = p.fadeOutFrames.exchange(-1);
        if (requestedFadeOut >= 0) {
            fadeOutTotal = std::max<uint64_t>(1, static_cast<uint64_t>(requestedFadeOut * framesPerGameFrame));
            fadeOutDone = 0;
        }

        // Decode one block.
        pcm.clear();
        const BlockRef& ref = blocks[blockIdx];
        const uint8_t* payload = data + ref.offset + kBlockHeaderSize;
        size_t frames = decodeBlock(info, payload, ref.bytesPerChannel, pcm, hist);

        // Trim: loop-seek skip at the front, stream/loop end at the back.
        size_t begin = 0;
        if (blockSkip > 0) {
            begin = std::min<size_t>(blockSkip, frames);
            blockSkip = 0;
        }
        size_t end = frames;
        const uint64_t blockEndSample = static_cast<uint64_t>(ref.firstSample) + frames;
        bool reachedEnd = false;
        if (blockEndSample >= endSample) {
            end = static_cast<size_t>(endSample - ref.firstSample);
            reachedEnd = true;
        }
        if (end < begin) {
            end = begin;
        }

        int16_t* chunk = pcm.data() + begin * 2;
        size_t chunkFrames = end - begin;

        if (needResample && chunkFrames > 0) {
            resampler.process(chunk, chunkFrames, resampled);
            chunk = resampled.data();
            chunkFrames = resampled.size() / 2;
        }

        // Fades + bus volume, per frame group (cheap: one gain per 256 frames).
        const float bus = p.volume.load(std::memory_order_relaxed);
        for (size_t f = 0; f < chunkFrames; f += 256) {
            const size_t n = std::min<size_t>(256, chunkFrames - f);
            float gain = bus;
            if (fadeInTotal > 0 && played + f < fadeInTotal) {
                gain *= static_cast<float>(played + f) / static_cast<float>(fadeInTotal);
            }
            if (fadeOutTotal > 0) {
                const uint64_t pos = fadeOutDone + f;
                gain *= pos >= fadeOutTotal ? 0.0f : 1.0f - static_cast<float>(pos) / static_cast<float>(fadeOutTotal);
            }
            applyGain(chunk + f * 2, n * 2, gain);
        }

        if (chunkFrames > 0) {
            pushPaced(chunk, chunkFrames);
        }
        played += chunkFrames;
        if (fadeOutTotal > 0) {
            fadeOutDone += chunkFrames;
            if (fadeOutDone >= fadeOutTotal) {
                finished = true;
                break;
            }
        }

        if (reachedEnd) {
            if (!useLoop) {
                finished = true;
                break;
            }
            // Seek to loopStart: find its block, reset AFC history (the
            // encoder resets per block boundary anyway) and skip inside it.
            size_t target = 0;
            for (size_t i = 0; i < blocks.size(); ++i) {
                if (blocks[i].firstSample <= info.loopStart) {
                    target = i;
                } else {
                    break;
                }
            }
            blockIdx = target;
            blockSkip = info.loopStart - blocks[target].firstSample;
            hist[0][0] = hist[0][1] = hist[1][0] = hist[1][1] = 0;
        } else {
            ++blockIdx;
            if (blockIdx >= blocks.size()) {
                finished = true;
            }
        }
    }

    p.playing.store(false);
    PL_LOG_INFO("game.audio", "AST '%s': stream ended (%s)", p.path.c_str(),
                p.quit.load() ? "stopped" : (fadeOutTotal > 0 ? "faded out" : "end of file"));
}

void joinThreadLocked(Player& p) {
    if (p.thread != nullptr) {
        p.quit.store(true);
        p.thread->join();
        delete p.thread;
        p.thread = nullptr;
    }
    p.quit.store(false);
    p.playing.store(false);
}

} // namespace

// --- decoding ------------------------------------------------------------------

void decodeAfcFrames(const uint8_t* src, size_t frames, int16_t* dst, int16_t hist[2]) {
    int32_t h1 = hist[0];
    int32_t h2 = hist[1];
    for (size_t f = 0; f < frames; ++f) {
        const uint8_t header = src[f * kAfcFrameBytes];
        const int32_t scale = 1 << (header >> 4);
        const int coefIdx = header & 0x0F;
        const int32_t c1 = kAfcCoef[coefIdx][0];
        const int32_t c2 = kAfcCoef[coefIdx][1];
        for (size_t i = 0; i < kAfcFrameSamples; ++i) {
            const uint8_t byte = src[f * kAfcFrameBytes + 1 + i / 2];
            int32_t nibble = (i & 1) == 0 ? (byte >> 4) : (byte & 0x0F);
            if (nibble >= 8) {
                nibble -= 16;
            }
            const int32_t sample = clamp16((((nibble * scale) << 11) + c1 * h1 + c2 * h2) >> 11);
            dst[f * kAfcFrameSamples + i] = static_cast<int16_t>(sample);
            h2 = h1;
            h1 = sample;
        }
    }
    hist[0] = static_cast<int16_t>(h1);
    hist[1] = static_cast<int16_t>(h2);
}

bool parseAstHeader(const uint8_t* data, size_t size, AstInfo& out) {
    if (data == nullptr || size < kHeaderSize) {
        return false;
    }
    if (std::memcmp(data, "STRM", 4) != 0) {
        return false;
    }
    out.codec = rd16(data + 0x08);
    const uint16_t bits = rd16(data + 0x0A);
    out.channels = rd16(data + 0x0C);
    out.looped = rd16(data + 0x0E) != 0;
    out.sampleRate = rd32(data + 0x10);
    out.numSamples = rd32(data + 0x14);
    out.loopStart = rd32(data + 0x18);
    out.loopEnd = rd32(data + 0x1C);

    if ((out.codec != 0 && out.codec != 1) || bits != 16) {
        return false;
    }
    if (out.channels == 0 || out.channels > 8) {
        return false;
    }
    if (out.sampleRate < 4000 || out.sampleRate > 96000) {
        return false;
    }
    if (out.looped && out.loopEnd <= out.loopStart) {
        out.looped = false;  // degenerate loop → play once
    }
    return true;
}

bool decodeAstToStereo(const uint8_t* data, size_t size, AstInfo& info, std::vector<int16_t>& out) {
    out.clear();
    if (!parseAstHeader(data, size, info)) {
        return false;
    }
    int16_t hist[2][2] = {{0, 0}, {0, 0}};
    uint32_t total = 0;
    forEachBlock(data, size, info, kHeaderSize, [&](const uint8_t* payload, size_t bytesPerChannel) {
        total += static_cast<uint32_t>(decodeBlock(info, payload, bytesPerChannel, out, hist));
        return true;
    });
    if (info.numSamples == 0 || info.numSamples > total) {
        info.numSamples = total;
    }
    out.resize(static_cast<size_t>(info.numSamples) * 2);
    return true;
}

// --- player ----------------------------------------------------------------------

bool streamPathForLabel(const char* label, char* out, size_t outSize) {
    if (label == nullptr || out == nullptr || outSize == 0) {
        return false;
    }
    for (const LabelEntry& e : kLabelTable) {
        if (stringCaseCompare(e.label, label) == 0) {
            std::snprintf(out, outSize, "/AudioRes/Stream/%s.ast", e.file);
            return true;
        }
    }
    std::snprintf(out, outSize, "/AudioRes/Stream/%s.ast", label);
    return false;
}

bool startStream(const char* virtualPath, bool loop, int fadeInFrames) {
    if (virtualPath == nullptr) {
        return false;
    }
    Player& p = player();
    std::lock_guard<std::mutex> lock(p.mutex);

    joinThreadLocked(p);

    if (!Platform::Audio::isInitialized()) {
        PL_LOG_INFO("game.audio", "startStream('%s'): audio not initialized — silent", virtualPath);
        return false;
    }

    std::vector<uint8_t> file = Platform::Filesystem::readFile(virtualPath);
    AstInfo info;
    if (file.empty() || !parseAstHeader(file.data(), file.size(), info)) {
        PL_LOG_WARN("game.audio", "startStream('%s'): %s", virtualPath,
                    file.empty() ? "file not found in the assets tree" : "not a valid AST (STRM) file");
        return false;
    }

    p.path = virtualPath;
    p.pathSnapshot = p.path;
    p.playing.store(true);
    p.fadeOutFrames.store(-1);
    p.quit.store(false);

    p.thread = new Platform::Threading::Thread("ast-stream", [file = std::move(file), info, loop, fadeInFrames]() mutable {
        decodeThreadMain(std::move(file), info, loop, fadeInFrames);
    });
    return true;
}

void stopStream(int fadeOutFrames) {
    Player& p = player();
    if (!p.playing.load()) {
        return;
    }
    if (fadeOutFrames <= 0) {
        std::lock_guard<std::mutex> lock(p.mutex);
        joinThreadLocked(p);
        return;
    }
    p.fadeOutFrames.store(fadeOutFrames);
}

bool isStreamPlaying() {
    return player().playing.load();
}

const char* currentStreamPath() {
    Player& p = player();
    return p.playing.load() ? p.pathSnapshot.c_str() : "";
}

void setStreamVolume(float volume) {
    player().volume.store(std::clamp(volume, 0.0f, 1.0f));
}

void shutdownStreams() {
    Player& p = player();
    std::lock_guard<std::mutex> lock(p.mutex);
    joinThreadLocked(p);
}

} // namespace compat::audio
