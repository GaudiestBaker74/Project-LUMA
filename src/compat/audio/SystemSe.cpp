// SE_SY_GAME_START — the title-screen A+B decide sound.
//
// The game already calls MR::startSystemSE("SE_SY_GAME_START") once, when
// both buttons are down, then leaves the logo nerve (so holding A+B does not
// retrigger). This file is the missing playback half: read the original wave
// out of AudioRes/Waves/B21kawa_0.aw using the slice SMR.szs describes, decode
// it with the existing DSP ADPCM / PCM helpers, and mix it once via
// Platform::Audio::playOneShot. No replacement tone is generated.

#include "compat/audio/SystemSe.h"

#include "compat/dsp/Adpcm.h"
#include "platform/Audio/Audio.h"
#include "platform/Filesystem/Filesystem.h"
#include "platform/Log/Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace compat::audio {
namespace {

uint32_t be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

uint16_t be16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint32_t>(p[0]) << 8) | static_cast<uint32_t>(p[1]));
}

float bef32(const uint8_t* p) {
    const uint32_t u = be32(p);
    float f = 0.0f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

bool inRange(size_t off, size_t need, size_t size) {
    return off <= size && need <= size - off;
}

// Yaz0 (SZS). Same back-reference rule as JKRDecomp::decodeSZS: the 12-bit
// field stores distance-1.
bool decompressYaz0(const uint8_t* src, size_t srcSize, std::vector<uint8_t>& out) {
    if (srcSize < 16 || std::memcmp(src, "Yaz0", 4) != 0) {
        return false;
    }
    const uint32_t dstSize = be32(src + 4);
    if (dstSize == 0 || dstSize > 32u * 1024u * 1024u) {
        return false;
    }
    out.assign(dstSize, 0);
    size_t sp = 16;
    size_t dp = 0;
    while (dp < dstSize) {
        if (sp >= srcSize) {
            return false;
        }
        uint8_t flags = src[sp++];
        for (int bit = 0; bit < 8 && dp < dstSize; ++bit) {
            if ((flags & 0x80) != 0) {
                if (sp >= srcSize) {
                    return false;
                }
                out[dp++] = src[sp++];
            } else {
                if (sp + 1 >= srcSize) {
                    return false;
                }
                const uint8_t b1 = src[sp++];
                const uint8_t b2 = src[sp++];
                const size_t dist = (static_cast<size_t>(b1 & 0x0F) << 8 | b2) + 1;
                size_t len = b1 >> 4;
                if (len == 0) {
                    if (sp >= srcSize) {
                        return false;
                    }
                    len = static_cast<size_t>(src[sp++]) + 0x12;
                } else {
                    len += 2;
                }
                if (dist == 0 || dist > dp) {
                    return false;
                }
                for (size_t i = 0; i < len && dp < dstSize; ++i) {
                    out[dp] = out[dp - dist];
                    ++dp;
                }
            }
            flags <<= 1;
        }
    }
    return true;
}

struct WaveSlice {
    uint8_t format = 0;
    float sampleRate = 0.0f;
    uint32_t offset = 0;
    uint32_t length = 0;
    uint32_t sampleCount = 0;
    char archive[0x71] = {};
};

// Section-relative offset, or file-relative if it falls outside the section.
const uint8_t* resolveOff(const uint8_t* file, size_t fileSize, const uint8_t* section, size_t sectionSize,
                          uint32_t off, size_t need) {
    if (section != nullptr && inRange(off, need, sectionSize)) {
        return section + off;
    }
    if (inRange(off, need, fileSize)) {
        return file + off;
    }
    return nullptr;
}

int findSystemNameIndex(const uint8_t* file, size_t fileSize, const uint8_t* bstn, size_t bstnSize, const char* name) {
    const size_t nameLen = std::strlen(name);
    if (nameLen == 0 || bstn == nullptr) {
        return -1;
    }
    const size_t bstnBase = static_cast<size_t>(bstn - file);
    for (size_t i = 0; i + nameLen + 1 <= fileSize; ++i) {
        if (std::memcmp(file + i, name, nameLen) != 0 || file[i + nameLen] != 0) {
            continue;
        }
        const uint32_t fileOff = static_cast<uint32_t>(i);
        const uint32_t secOff = (i >= bstnBase) ? static_cast<uint32_t>(i - bstnBase) : 0xFFFFFFFFu;
        // A name-table slot is a BE u32 pointing at this string.
        for (size_t p = 0; p + 4 <= fileSize; ++p) {
            const uint32_t ptr = be32(file + p);
            if (ptr != fileOff && ptr != secOff) {
                continue;
            }
            for (int k = 0; k < 512; ++k) {
                if (p < static_cast<size_t>(8 + k * 4)) {
                    break;
                }
                const size_t table = p - static_cast<size_t>(8 + k * 4);
                if (!inRange(table, 8, fileSize)) {
                    break;
                }
                if (table < bstnBase || table >= bstnBase + bstnSize) {
                    continue;
                }
                const uint32_t count = be32(file + table);
                if (count == 0 || count > 2000 || static_cast<uint32_t>(k) >= count) {
                    continue;
                }
                const uint8_t* cat = resolveOff(file, fileSize, bstn, bstnSize, be32(file + table + 4), 7);
                if (cat == nullptr || std::memcmp(cat, "SYSTEM", 6) != 0) {
                    continue;
                }
                return k;
            }
        }
    }
    return -1;
}

// First E1 (bank, program) in the SE body. C3/C7/C1 operands are skipped so a
// pointer byte is not mistaken for the opcode. The shared tempo stub that a
// C3 calls does not hold the instrument.
bool findBankProgram(const uint8_t* bms, size_t len, int& bank, int& program) {
    size_t i = 0;
    while (i < len && i < 48) {
        const uint8_t op = bms[i];
        if (op == 0xE1 && i + 2 < len) {
            bank = bms[i + 1];
            program = bms[i + 2];
            return true;
        }
        if (op == 0xC3 || op == 0xC7) {
            i += 4;
            continue;
        }
        if (op == 0xC1) {
            i += 5;
            continue;
        }
        ++i;
    }
    return false;
}

bool waveFromInst(const uint8_t* inst, size_t avail, uint16_t& waveId) {
    if (avail < 0x14 + 0x18 || std::memcmp(inst, "Inst", 4) != 0) {
        return false;
    }
    const uint32_t keyboards = be32(inst + 0x10);
    if (keyboards == 0 || keyboards > 128) {
        return false;
    }
    if (!inRange(0x14, 0x18, avail)) {
        return false;
    }
    // First keyboard region. A one-note system SE uses a single region.
    waveId = be16(inst + 0x14 + 0x0E);
    return true;
}

bool waveIdForProgram(const uint8_t* ibnk, size_t ibnkSize, int program, uint16_t& waveId) {
    if (program < 0 || ibnkSize < 16 || std::memcmp(ibnk, "IBNK", 4) != 0) {
        return false;
    }
    for (size_t i = 0; i + 12 <= ibnkSize; ++i) {
        if (std::memcmp(ibnk + i, "LIST", 4) != 0) {
            continue;
        }
        const uint32_t count = be32(ibnk + i + 8);
        if (count == 0 || count > 256 || static_cast<uint32_t>(program) >= count) {
            continue;
        }
        const size_t entry = i + 0x0C + static_cast<size_t>(program) * 4;
        if (!inRange(entry, 4, ibnkSize)) {
            continue;
        }
        const uint32_t raw = be32(ibnk + entry);
        if (raw == 0) {
            continue;
        }
        const uint8_t* tries[2] = {nullptr, nullptr};
        size_t navail[2] = {0, 0};
        int ntry = 0;
        if (inRange(raw, 4, ibnkSize)) {
            tries[ntry] = ibnk + raw;
            navail[ntry] = ibnkSize - raw;
            ++ntry;
        }
        if (inRange(i + raw, 4, ibnkSize)) {
            tries[ntry] = ibnk + i + raw;
            navail[ntry] = ibnkSize - (i + raw);
            ++ntry;
        }
        for (int t = 0; t < ntry; ++t) {
            if (waveFromInst(tries[t], navail[t], waveId)) {
                return true;
            }
        }
    }
    return false;
}

bool readWave(const uint8_t* wsys, size_t wsysSize, uint32_t waveOff, WaveSlice& out) {
    if (!inRange(waveOff, 0x20, wsysSize)) {
        return false;
    }
    const uint8_t* w = wsys + waveOff;
    out.format = w[1];
    out.sampleRate = bef32(w + 4);
    out.offset = be32(w + 8);
    out.length = be32(w + 0x0C);
    out.sampleCount = be32(w + 0x1C);
    if (out.format > 7 || out.length > 8u * 1024u * 1024u || out.sampleCount > 48000u * 10u) {
        return false;
    }
    if (!std::isfinite(out.sampleRate) || out.sampleRate < 1.0f || out.sampleRate > 192000.0f) {
        out.sampleRate = 32000.0f;
    }
    return out.sampleCount > 0 || out.length > 0;
}

uint16_t ctrlWaveId(const uint8_t* wsys, size_t wsysSize, uint32_t ctrlGroupOff, uint32_t group, uint32_t index,
                    uint16_t fallback) {
    if (!inRange(ctrlGroupOff, 0x10, wsysSize)) {
        return fallback;
    }
    const uint32_t groups = be32(wsys + ctrlGroupOff + 8);
    if (group >= groups || groups > 64) {
        return fallback;
    }
    const uint32_t sceneOff = be32(wsys + ctrlGroupOff + 0x0C + group * 4);
    if (!inRange(sceneOff, 0x10, wsysSize)) {
        return fallback;
    }
    const uint32_t ctrlOff = be32(wsys + sceneOff + 0x0C);
    if (!inRange(ctrlOff, 8 + (index + 1) * 4, wsysSize)) {
        return fallback;
    }
    const uint32_t cwOff = be32(wsys + ctrlOff + 8 + index * 4);
    if (!inRange(cwOff, 4, wsysSize)) {
        return fallback;
    }
    return static_cast<uint16_t>(be32(wsys + cwOff) & 0xFFFF);
}

bool findWaveInWsys(const uint8_t* wsys, size_t wsysSize, uint16_t waveId, bool anyId, WaveSlice& out) {
    if (wsysSize < 0x18) {
        return false;
    }
    const uint32_t bankOff = be32(wsys + 0x10);
    const uint32_t ctrlOff = be32(wsys + 0x14);
    if (!inRange(bankOff, 8, wsysSize)) {
        return false;
    }
    const uint32_t groups = be32(wsys + bankOff + 4);
    if (groups == 0 || groups > 64) {
        return false;
    }
    for (uint32_t g = 0; g < groups; ++g) {
        const uint32_t archOff = be32(wsys + bankOff + 8 + g * 4);
        if (!inRange(archOff, 0x78, wsysSize)) {
            continue;
        }
        const uint32_t nwave = be32(wsys + archOff + 0x70);
        if (nwave == 0 || nwave > 4096 || !inRange(archOff + 0x74, nwave * 4, wsysSize)) {
            continue;
        }
        for (uint32_t j = 0; j < nwave; ++j) {
            const uint16_t id = ctrlWaveId(wsys, wsysSize, ctrlOff, g, j, static_cast<uint16_t>(j));
            if (!anyId && id != waveId && j != waveId) {
                continue;
            }
            if (anyId && j != 0) {
                continue;
            }
            const uint32_t waveOff = be32(wsys + archOff + 0x74 + j * 4);
            if (!readWave(wsys, wsysSize, waveOff, out)) {
                continue;
            }
            std::memcpy(out.archive, wsys + archOff, 0x70);
            out.archive[0x70] = 0;
            return true;
        }
    }
    return false;
}

bool filenameHas(const char* archive, const char* needle) {
    return archive[0] != 0 && std::strstr(archive, needle) != nullptr;
}

bool findSlice(const uint8_t* file, size_t fileSize, const char* seName, WaveSlice& out) {
    if (fileSize < 0x44 || std::memcmp(file, "AA_<", 4) != 0) {
        return false;
    }
    const uint32_t bstnStart = be32(file + 0x14);
    const uint32_t bstnEnd = be32(file + 0x18);
    const uint32_t bscStart = be32(file + 0x20);
    const uint32_t bscEnd = be32(file + 0x24);
    if (!inRange(bstnStart, 0x20, fileSize) || bstnEnd <= bstnStart || bstnEnd > fileSize) {
        return false;
    }
    if (!inRange(bscStart, 0x10, fileSize) || bscEnd <= bscStart || bscEnd > fileSize) {
        return false;
    }
    const uint8_t* bstn = file + bstnStart;
    const size_t bstnSize = bstnEnd - bstnStart;
    const uint8_t* bsc = file + bscStart;
    const size_t bscSize = bscEnd - bscStart;

    const int index = findSystemNameIndex(file, fileSize, bstn, bstnSize, seName);
    int bank = -1;
    int program = -1;
    if (index >= 0) {
        const uint8_t* table = resolveOff(file, fileSize, bsc, bscSize, be32(bsc + 8), 8);
        if (table != nullptr) {
            const uint32_t count = be32(table);
            if (static_cast<uint32_t>(index) < count && count < 2000 &&
                inRange(static_cast<size_t>(table - file), 4 + (static_cast<size_t>(index) + 1) * 4, fileSize)) {
                const uint32_t bmsOff = be32(table + 4 + static_cast<size_t>(index) * 4);
                const uint8_t* bms = resolveOff(file, fileSize, bsc, bscSize, bmsOff, 3);
                if (bms != nullptr) {
                    const size_t bmsLen = static_cast<size_t>((file + fileSize) - bms);
                    findBankProgram(bms, bmsLen, bank, program);
                }
            }
        }
    }

    // bnk entries follow the ws entries. IBNK bank id is the BE s32 at +8.
    size_t cursor = 0x28;
    uint16_t waveId = 0;
    bool haveWaveId = false;
    if (bank >= 0) {
        size_t scan = 0x28;
        while (scan + 16 <= fileSize && std::memcmp(file + scan, "ws  ", 4) == 0) {
            scan += 16;
        }
        while (scan + 12 <= fileSize && std::memcmp(file + scan, "bnk ", 4) == 0) {
            const uint32_t ibnkOff = be32(file + scan + 8);
            scan += 12;
            if (!inRange(ibnkOff, 16, fileSize) || std::memcmp(file + ibnkOff, "IBNK", 4) != 0) {
                continue;
            }
            if (static_cast<int>(be32(file + ibnkOff + 8)) != bank) {
                continue;
            }
            size_t ibnkSize = fileSize - ibnkOff;
            if (ibnkSize > 256 * 1024) {
                ibnkSize = 256 * 1024;
            }
            if (waveIdForProgram(file + ibnkOff, ibnkSize, program, waveId)) {
                haveWaveId = true;
                break;
            }
        }
    }

    WaveSlice kawaFallback{};
    bool haveKawa = false;
    while (cursor + 16 <= fileSize && std::memcmp(file + cursor, "ws  ", 4) == 0) {
        const uint32_t wsysId = be32(file + cursor + 4);
        const uint32_t wsysOff = be32(file + cursor + 8);
        cursor += 16;
        if (!inRange(wsysOff, 0x18, fileSize)) {
            continue;
        }
        size_t wsysSize = fileSize - wsysOff;
        if (wsysSize > 1024 * 1024) {
            wsysSize = 1024 * 1024;
        }
        WaveSlice slice{};
        const bool wantThis = haveWaveId && (bank < 0 || static_cast<int>(wsysId) == bank);
        if (wantThis && findWaveInWsys(file + wsysOff, wsysSize, waveId, false, slice)) {
            out = slice;
            return true;
        }
        if (!haveKawa && findWaveInWsys(file + wsysOff, wsysSize, 0, true, slice) &&
            filenameHas(slice.archive, "B21kawa_0")) {
            kawaFallback = slice;
            haveKawa = true;
        }
        if (haveWaveId && findWaveInWsys(file + wsysOff, wsysSize, waveId, false, slice) &&
            filenameHas(slice.archive, "B21kawa")) {
            out = slice;
            return true;
        }
    }
    if (haveWaveId) {
        // Bank id on the ws entry may not match. Search every WSYS for the wave.
        cursor = 0x28;
        while (cursor + 16 <= fileSize && std::memcmp(file + cursor, "ws  ", 4) == 0) {
            const uint32_t wsysOff = be32(file + cursor + 8);
            cursor += 16;
            if (!inRange(wsysOff, 0x18, fileSize)) {
                continue;
            }
            size_t wsysSize = fileSize - wsysOff;
            if (wsysSize > 1024 * 1024) {
                wsysSize = 1024 * 1024;
            }
            if (findWaveInWsys(file + wsysOff, wsysSize, waveId, false, out)) {
                return true;
            }
        }
    }
    if (haveKawa) {
        PL_LOG_WARN("game.audio", "SE_SY_GAME_START: sound-table wave unresolved, using wave 0 of %s",
                    kawaFallback.archive);
        out = kawaFallback;
        return true;
    }
    return false;
}

bool decodeSlice(const uint8_t* aw, size_t awSize, const WaveSlice& slice, int outRate, std::vector<int16_t>& stereo) {
    if (aw == nullptr || slice.offset > awSize) {
        return false;
    }
    const size_t avail = awSize - slice.offset;
    size_t length = slice.length == 0 ? avail : slice.length;
    if (length > avail) {
        length = avail;
    }
    const uint8_t* src = aw + slice.offset;
    uint32_t sampleCount = slice.sampleCount;
    if (sampleCount == 0) {
        if (slice.format == 0) {
            sampleCount = static_cast<uint32_t>((length / 9) * 16);
        } else if (slice.format == 3) {
            sampleCount = static_cast<uint32_t>(length / 2);
        } else if (slice.format == 2) {
            sampleCount = static_cast<uint32_t>(length);
        }
    }
    if (sampleCount == 0 || sampleCount > 48000u * 10u) {
        return false;
    }

    std::vector<int16_t> mono(sampleCount);
    if (slice.format == 0) {
        compat::dsp::AdpcmState state;
        int16_t block[compat::dsp::kAdpcmBlockSamples];
        uint32_t done = 0;
        size_t pos = 0;
        while (done < sampleCount) {
            if (pos + compat::dsp::kAdpcmBlockBytes > length) {
                return false;
            }
            compat::dsp::adpcmDecodeBlock(src + pos, block, state);
            pos += compat::dsp::kAdpcmBlockBytes;
            const uint32_t n = std::min<uint32_t>(static_cast<uint32_t>(compat::dsp::kAdpcmBlockSamples), sampleCount - done);
            std::memcpy(mono.data() + done, block, n * sizeof(int16_t));
            done += n;
        }
    } else if (slice.format == 3) {
        if (length < static_cast<size_t>(sampleCount) * 2) {
            return false;
        }
        for (uint32_t i = 0; i < sampleCount; ++i) {
            mono[i] = static_cast<int16_t>(be16(src + i * 2));
        }
    } else if (slice.format == 2) {
        if (length < sampleCount) {
            return false;
        }
        for (uint32_t i = 0; i < sampleCount; ++i) {
            mono[i] = static_cast<int16_t>((static_cast<int32_t>(src[i]) - 128) << 8);
        }
    } else {
        PL_LOG_WARN("game.audio", "SE_SY_GAME_START wave format %u is not ADPCM/PCM", slice.format);
        return false;
    }

    if (outRate <= 0) {
        outRate = 32000;
    }
    const double rate = slice.sampleRate < 1.0f ? static_cast<double>(outRate) : static_cast<double>(slice.sampleRate);
    if (std::fabs(rate - static_cast<double>(outRate)) < 0.5) {
        stereo.resize(static_cast<size_t>(sampleCount) * 2);
        for (uint32_t i = 0; i < sampleCount; ++i) {
            stereo[static_cast<size_t>(i) * 2] = mono[i];
            stereo[static_cast<size_t>(i) * 2 + 1] = mono[i];
        }
        return true;
    }
    const double step = rate / static_cast<double>(outRate);
    int frames = static_cast<int>(static_cast<double>(sampleCount) / step);
    if (frames < 1) {
        frames = 1;
    }
    stereo.resize(static_cast<size_t>(frames) * 2);
    for (int i = 0; i < frames; ++i) {
        const double srcPos = static_cast<double>(i) * step;
        const int i0 = static_cast<int>(srcPos);
        const int i1 = std::min(i0 + 1, static_cast<int>(sampleCount) - 1);
        const double t = srcPos - static_cast<double>(i0);
        const double s = static_cast<double>(mono[i0]) * (1.0 - t) + static_cast<double>(mono[i1]) * t;
        const auto sample = static_cast<int16_t>(s);
        stereo[static_cast<size_t>(i) * 2] = sample;
        stereo[static_cast<size_t>(i) * 2 + 1] = sample;
    }
    return true;
}

std::vector<uint8_t> loadFirst(const std::vector<std::string>& paths) {
    for (const std::string& path : paths) {
        std::vector<uint8_t> data = Platform::Filesystem::readFile(path);
        if (!data.empty()) {
            PL_LOG_INFO("game.audio", "loaded '%s' (%zu bytes)", path.c_str(), data.size());
            return data;
        }
    }
    return {};
}

std::string baseName(const char* path) {
    const char* slash = std::strrchr(path, '/');
    const char* bslash = std::strrchr(path, '\\');
    const char* leaf = path;
    if (slash != nullptr && slash + 1 > leaf) {
        leaf = slash + 1;
    }
    if (bslash != nullptr && bslash + 1 > leaf) {
        leaf = bslash + 1;
    }
    return leaf;
}

}  // namespace

bool decodeNamedSystemSe(const uint8_t* baa, size_t baaSize, const uint8_t* aw, size_t awSize, const char* seName,
                         int outRate, std::vector<int16_t>& stereoOut) {
    stereoOut.clear();
    if (baa == nullptr || aw == nullptr || seName == nullptr) {
        return false;
    }
    std::vector<uint8_t> owned;
    const uint8_t* file = baa;
    size_t fileSize = baaSize;
    if (baaSize >= 4 && std::memcmp(baa, "Yaz0", 4) == 0) {
        if (!decompressYaz0(baa, baaSize, owned)) {
            return false;
        }
        file = owned.data();
        fileSize = owned.size();
    }
    WaveSlice slice;
    if (!findSlice(file, fileSize, seName, slice)) {
        return false;
    }
    return decodeSlice(aw, awSize, slice, outRate, stereoOut);
}

bool playNamedSystemSe(const char* name) {
    if (name == nullptr || std::strcmp(name, "SE_SY_GAME_START") != 0) {
        return false;
    }

    static std::vector<int16_t> cached;
    static bool cachedOk = false;
    if (!cachedOk) {
        const std::vector<uint8_t> smr = loadFirst({
            "/AudioRes/SMR.szs",
            "/AudioRes/Info/SMR.szs",
            "/AudioRes/Banks/SMR.szs",
            "/AudioRes/JaiRes/SMR.szs",
        });
        if (smr.empty()) {
            PL_LOG_WARN("game.audio", "SE_SY_GAME_START: SMR.szs not found under AudioRes");
            return false;
        }
        std::vector<uint8_t> baa;
        const uint8_t* file = smr.data();
        size_t fileSize = smr.size();
        if (smr.size() >= 4 && std::memcmp(smr.data(), "Yaz0", 4) == 0) {
            if (!decompressYaz0(smr.data(), smr.size(), baa)) {
                PL_LOG_WARN("game.audio", "SE_SY_GAME_START: SMR.szs Yaz0 decode failed");
                return false;
            }
            file = baa.data();
            fileSize = baa.size();
        }
        WaveSlice slice;
        if (!findSlice(file, fileSize, name, slice)) {
            PL_LOG_WARN("game.audio", "SE_SY_GAME_START: sound table has no playable wave");
            return false;
        }
        std::vector<std::string> wavePaths;
        if (slice.archive[0] != 0) {
            if (slice.archive[0] == '/') {
                wavePaths.emplace_back(slice.archive);
            } else {
                wavePaths.emplace_back(std::string("/") + slice.archive);
            }
            wavePaths.emplace_back("/AudioRes/Waves/" + baseName(slice.archive));
        }
        wavePaths.emplace_back("/AudioRes/Waves/B21kawa_0.aw");
        const std::vector<uint8_t> aw = loadFirst(wavePaths);
        if (aw.empty()) {
            PL_LOG_WARN("game.audio", "SE_SY_GAME_START: %s not found",
                        slice.archive[0] ? slice.archive : "B21kawa_0.aw");
            return false;
        }
        const int rate = Platform::Audio::inputFreq() > 0 ? Platform::Audio::inputFreq() : 32000;
        if (!decodeSlice(aw.data(), aw.size(), slice, rate, cached) || cached.size() < 2) {
            cached.clear();
            PL_LOG_WARN("game.audio", "SE_SY_GAME_START: could not decode %s (format %u)", slice.archive,
                        slice.format);
            return false;
        }
        cachedOk = true;
        PL_LOG_INFO("game.audio", "SE_SY_GAME_START decoded from '%s' format %u rate %.0f frames %zu",
                    slice.archive, slice.format, static_cast<double>(slice.sampleRate), cached.size() / 2);
    }

    if (!Platform::Audio::playOneShot(cached.data(), static_cast<int>(cached.size() / 2), 1.0f)) {
        PL_LOG_WARN("game.audio", "SE_SY_GAME_START: playOneShot failed");
        return false;
    }
    return true;
}

}  // namespace compat::audio
