// =============================================================================
// M9.5.1: JKernel archive stack tests (headless, no GPU).
//
// Exercises the real vendored/patched RARC pipeline the game uses for every
// layout/model/message archive:
//
//   - JKRDecomp (patched): Yaz0 (SZS) decode — literal runs and back-refs,
//     decoded through the synchronous host path.
//   - JKRMemArchive::mountFixed: an in-memory RARC built by this file
//     (big-endian metadata, 20-byte on-disk file entries) is converted to
//     host layout (compat/jsystem/RarcHost) and its resources fetched.
//   - JKRArchive::mount over the compat DVD layer: the full chain
//     DVDConvertPathToEntrynum -> JKRDvdFile -> JKRDvdRipper (raw + Yaz0
//     streaming decode) -> JKRMemArchive, against a synthetic assets tree
//     (same pattern as dvd_test.cpp).
//   - Failure paths: non-RARC garbage and Yaz0 with a truncated payload must
//     fail the mount, not crash or read out of bounds.
//
// The synthetic RARC mirrors the real format (see docs/assets.md): 0x20-byte
// header, 0x20-byte info block, 0x10-byte dir entries, 0x14-byte file
// entries, string table, 0x20-aligned file data — all big-endian.
// =============================================================================

#include "tests/test_runner.h"

#include "compat/dvd/DVDCompat.h"

#include "platform/Filesystem/Filesystem.h"

#include <JSystem/JKernel/JKRArchive.hpp>
#include <JSystem/JKernel/JKRDecomp.hpp>
#include <JSystem/JKernel/JKRDvdRipper.hpp>
#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#include <JSystem/JKernel/JKRMemArchive.hpp>

// The game-facing FileUtil entry points (implemented over this stack in
// compat/game/SceneCompat.cpp). Forward-declared instead of including
// Game/Util/FileUtil.hpp, which pulls the whole FileLoader/GameData tree.
namespace MR {
    void* loadToMainRAM(const char*, u8*, JKRHeap*, JKRDvdRipper::EAllocDirection);
    JKRMemArchive* mountArchive(const char*, JKRHeap*);
}

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Big-endian writers + the CArcName hash (JKRArchivePri.cpp: h = c + h*3).
// ---------------------------------------------------------------------------

void put16(std::vector<u8>& v, u16 value) {
    v.push_back(static_cast<u8>(value >> 8));
    v.push_back(static_cast<u8>(value));
}

void put32(std::vector<u8>& v, u32 value) {
    v.push_back(static_cast<u8>(value >> 24));
    v.push_back(static_cast<u8>(value >> 16));
    v.push_back(static_cast<u8>(value >> 8));
    v.push_back(static_cast<u8>(value));
}

void patch32(std::vector<u8>& v, size_t offset, u32 value) {
    v[offset + 0] = static_cast<u8>(value >> 24);
    v[offset + 1] = static_cast<u8>(value >> 16);
    v[offset + 2] = static_cast<u8>(value >> 8);
    v[offset + 3] = static_cast<u8>(value);
}

u16 arcHash(const char* name) {
    u16 hash = 0;
    for (const char* p = name; *p != '\0'; ++p) {
        hash = static_cast<u16>(static_cast<u16>(tolower(*p)) + hash * 3);
    }
    return hash;
}

// ---------------------------------------------------------------------------
// Synthetic RARC builder: root dir with one file ("hello.txt"), one subdir
// ("dir", 4-char id 'TEST') with one file ("inner.bin").
// ---------------------------------------------------------------------------

// Layout: header(0x20) | info(0x20) | dirs(2*0x10) | files(3*0x14) |
//         strings | pad-to-0x20 | file data.
std::vector<u8> buildTestRarc() {
    // String table: "." | "dir" | "hello.txt" | "inner.bin"
    std::vector<u8> strings;
    const u32 offRootName = 0;  // "."
    strings.push_back('.'); strings.push_back('\0');
    const u32 offDirName = static_cast<u32>(strings.size());  // "dir"
    for (char c : std::string("dir")) strings.push_back(static_cast<u8>(c));
    strings.push_back('\0');
    const u32 offHello = static_cast<u32>(strings.size());  // "hello.txt"
    for (char c : std::string("hello.txt")) strings.push_back(static_cast<u8>(c));
    strings.push_back('\0');
    const u32 offInner = static_cast<u32>(strings.size());  // "inner.bin"
    for (char c : std::string("inner.bin")) strings.push_back(static_cast<u8>(c));
    strings.push_back('\0');

    const std::vector<u8> helloData{'h', 'e', 'l', 'l', 'o', ' ', 'r', 'a', 'r', 'c'};
    const std::vector<u8> innerData{0x01, 0x02, 0x03, 0x04, 0x05};

    const u32 nrDirs = 2;
    const u32 nrFiles = 3;
    const u32 dirOffset = 0x20;  // relative to the info block
    const u32 fileOffset = dirOffset + nrDirs * 0x10;
    const u32 stringTableOffset = fileOffset + nrFiles * 0x14;
    const u32 stringTableSize = static_cast<u32>(strings.size());
    const u32 headerSize = 0x20;
    // File data starts 0x20-aligned after info+dirs+files+strings.
    const u32 tablesEnd = headerSize + 0x20 + stringTableOffset + stringTableSize;
    const u32 fileDataAbs = (tablesEnd + 0x1F) & ~0x1Fu;
    const u32 fileDataOffset = fileDataAbs - headerSize;  // header-relative

    const u32 helloDataOffset = 0;
    const u32 innerDataOffset = static_cast<u32>((helloData.size() + 0x1F) & ~0x1Fu);
    const u32 totalDataSize = innerDataOffset + static_cast<u32>(innerData.size());

    std::vector<u8> blob;
    // --- header ---
    blob.push_back('R'); blob.push_back('A'); blob.push_back('R'); blob.push_back('C');
    const size_t fileSizePos = blob.size();
    put32(blob, 0);  // patched at the end
    put32(blob, headerSize);
    put32(blob, fileDataOffset);
    put32(blob, totalDataSize);
    put32(blob, totalDataSize);  // mMRamDataSize (all MRAM)
    put32(blob, 0);              // mARamDataSize
    put32(blob, 0);
    // --- info block ---
    put32(blob, nrDirs);
    put32(blob, dirOffset);
    put32(blob, nrFiles);
    put32(blob, fileOffset);
    put32(blob, stringTableSize);
    put32(blob, stringTableOffset);
    put16(blob, nrFiles);  // mNextAvailableFileID
    put16(blob, 0);        // mFileIDIsIndex
    put32(blob, 0);
    // --- dir entries (0x10 each) ---
    // [0] root: 2 entries (hello.txt, dir), starting at file index 0.
    put32(blob, 0x524F4F54);  // 'ROOT'
    put32(blob, offRootName);
    put16(blob, 0);
    put16(blob, 2);
    put32(blob, 0);
    // [1] "dir": 1 entry (inner.bin), starting at file index 2.
    put32(blob, 0x54455354);  // 'TEST' (type id for findResType lookups)
    put32(blob, offDirName);
    put16(blob, arcHash("dir"));
    put16(blob, 1);
    put32(blob, 2);
    // --- file entries (0x14 each: id, hash, flag+nameoff(24), offset, size, pad) ---
    auto putFileEntry = [&](u16 id, const char* name, u32 nameOff, u8 flag, u32 dataOffset, u32 dataSize) {
        put16(blob, id);
        put16(blob, arcHash(name));
        blob.push_back(flag);
        blob.push_back(static_cast<u8>((nameOff >> 16) & 0xFF));
        put16(blob, static_cast<u16>(nameOff & 0xFFFF));
        put32(blob, dataOffset);
        put32(blob, dataSize);
        put32(blob, 0);  // padding (console runtime: mFileData)
    };
    // Root dir children: hello.txt (file), dir (folder -> dir index 1).
    putFileEntry(0, "hello.txt", offHello, 0x11 /* file|mram */, helloDataOffset, static_cast<u32>(helloData.size()));
    putFileEntry(0xFFFF, "dir", offDirName, 0x02 /* folder */, 1, 0x10);
    putFileEntry(1, "inner.bin", offInner, 0x11, innerDataOffset, static_cast<u32>(innerData.size()));
    // --- string table + padding ---
    blob.insert(blob.end(), strings.begin(), strings.end());
    while (blob.size() < fileDataAbs) {
        blob.push_back(0);
    }
    // --- file data ---
    blob.insert(blob.end(), helloData.begin(), helloData.end());
    while (blob.size() < fileDataAbs + innerDataOffset) {
        blob.push_back(0);
    }
    blob.insert(blob.end(), innerData.begin(), innerData.end());

    patch32(blob, fileSizePos, static_cast<u32>(blob.size()));
    return blob;
}

const std::vector<u8> kHelloData{'h', 'e', 'l', 'l', 'o', ' ', 'r', 'a', 'r', 'c'};
const std::vector<u8> kInnerData{0x01, 0x02, 0x03, 0x04, 0x05};

// ---------------------------------------------------------------------------
// Minimal Yaz0 encoder (greedy LZ77, 3..18-byte matches within 4096).
// Produces a stream the vendored decodeSZS/decompSZS_subroutine must accept.
// ---------------------------------------------------------------------------

std::vector<u8> yaz0Encode(const std::vector<u8>& src) {
    std::vector<u8> out;
    out.push_back('Y'); out.push_back('a'); out.push_back('z'); out.push_back('0');
    put32(out, static_cast<u32>(src.size()));
    for (int i = 0; i < 8; i++) out.push_back(0);  // reserved

    size_t pos = 0;
    while (pos < src.size()) {
        std::vector<u8> group;  // 8 tokens
        u8 control = 0;
        for (int bit = 0; bit < 8 && pos < src.size(); bit++) {
            // Longest back-reference: distance 1..4096, length 3..18.
            size_t bestLen = 0;
            size_t bestDist = 0;
            const size_t maxDist = pos < 4096 ? pos : 4096;
            const size_t maxLen = std::min<size_t>(18, src.size() - pos);
            if (maxLen >= 3) {
                for (size_t dist = 1; dist <= maxDist; dist++) {
                    size_t len = 0;
                    while (len < maxLen && src[pos - dist + len] == src[pos + len]) {
                        len++;
                    }
                    if (len > bestLen) {
                        bestLen = len;
                        bestDist = dist;
                        if (bestLen == maxLen) break;
                    }
                }
            }
            if (bestLen >= 3) {
                // Match token: bit 0.
                const u32 dist = static_cast<u32>(bestDist - 1);
                u32 lenNibble = static_cast<u32>(bestLen - 2);
                if (lenNibble > 15) {
                    group.push_back(static_cast<u8>((dist >> 8) & 0x0F));
                    group.push_back(static_cast<u8>(dist & 0xFF));
                    group.push_back(static_cast<u8>(bestLen - 0x12));
                } else {
                    group.push_back(static_cast<u8>((lenNibble << 4) | ((dist >> 8) & 0x0F)));
                    group.push_back(static_cast<u8>(dist & 0xFF));
                }
                pos += bestLen;
            } else {
                // Literal token: bit 1.
                control |= static_cast<u8>(0x80 >> bit);
                group.push_back(src[pos++]);
            }
        }
        out.push_back(control);
        out.insert(out.end(), group.begin(), group.end());
    }
    return out;
}

// ---------------------------------------------------------------------------
// Heap fixture: the archive stack allocates through JKRHeap, so the tests
// need the root heap. createRoot is idempotent (patched JKRExpHeap reuses an
// existing root), so tests that run after jkr_heap_test are safe.
// ---------------------------------------------------------------------------

JKRHeap* ensureHeap() {
    if (JKRHeap::sRootHeap == nullptr) {
        JKRExpHeap::createRoot(1, true);
    }
    JKRHeap::sRootHeap->becomeCurrentHeap();
    return JKRHeap::sRootHeap;
}

void writeFile(const std::string& path, const std::vector<u8>& data) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

const char* kTestRoot = "galaxy-pc-jkrarc";

std::string makeArchiveTree() {
    const std::string root = (fs::temp_directory_path() / kTestRoot).string();
    fs::remove_all(root);
    fs::create_directories(root + "/TestData");

    const std::vector<u8> rarc = buildTestRarc();
    writeFile(root + "/TestData/plain.arc", rarc);
    writeFile(root + "/TestData/comp.arc", yaz0Encode(rarc));
    writeFile(root + "/TestData/garbage.arc", {0xDE, 0xAD, 0xBE, 0xEF, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
                                               13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28,
                                               29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44});
    return root;
}

}  // namespace

// ---------------------------------------------------------------------------
// Yaz0 decode.
// ---------------------------------------------------------------------------

TEST_CASE(jkr_decomp_yaz0_roundtrip) {
    // Data with repetition so the encoder emits both literals and back-refs.
    std::vector<u8> src;
    for (int i = 0; i < 300; i++) {
        src.push_back(static_cast<u8>('A' + (i % 26)));
    }
    for (int i = 0; i < 100; i++) {
        src.push_back(0);
    }
    src.insert(src.end(), src.begin(), src.begin() + 200);  // long back-refs

    const std::vector<u8> encoded = yaz0Encode(src);
    REQUIRE(encoded.size() > 16);
    CHECK(encoded[0] == 'Y');
    CHECK(encoded[2] == 'z');  // Yaz0 = SZS
    CHECK(JKRDecomp::checkCompressed(const_cast<u8*>(encoded.data())) == JKR_COMPRESSION_SZS);

    // Contract note: the third parameter is the number of OUTPUT bytes to
    // produce (console callers pass JKRDecompExpandSize), not the stream
    // size — decodeSZS counts it down per written byte.
    std::vector<u8> dst(src.size(), 0xCC);
    JKRDecomp::decode(const_cast<u8*>(encoded.data()), dst.data(), static_cast<u32>(src.size()), 0);
    CHECK(dst == src);
}

TEST_CASE(jkr_decomp_yaz0_smaller_than_source) {
    // Repetitive input must actually compress (proves back-refs are emitted
    // and decoded, not just literal runs).
    std::vector<u8> src(512, 0x5A);
    const std::vector<u8> encoded = yaz0Encode(src);
    CHECK(encoded.size() < src.size());

    std::vector<u8> dst(src.size(), 0);
    JKRDecomp::decode(const_cast<u8*>(encoded.data()), dst.data(), static_cast<u32>(src.size()), 0);
    CHECK(dst == src);
}

// ---------------------------------------------------------------------------
// In-memory RARC mount (mountFixed) — exercises the RarcHost conversion.
// ---------------------------------------------------------------------------

TEST_CASE(jkr_mem_archive_mount_fixed) {
    JKRHeap* heap = ensureHeap();
    REQUIRE(heap != nullptr);

    const std::vector<u8> rarc = buildTestRarc();

    // mountFixed reads the blob through JKRHeap::findFromRoot, so it must be
    // heap-allocated (like a real MR::loadToMainRAM result).
    u8* blob = static_cast<u8*>(JKRHeap::alloc(static_cast<u32>(rarc.size()), 32, heap));
    REQUIRE(blob != nullptr);
    std::memcpy(blob, rarc.data(), rarc.size());

    JKRMemArchive archive;
    // BREAK_FLAG_1: the archive owns the blob and frees it on unmount.
    REQUIRE(archive.mountFixed(blob, JKR_MEM_BREAK_FLAG_1));
    CHECK(archive.mIsMounted);

    void* hello = archive.getResource("/hello.txt");
    REQUIRE(hello != nullptr);
    CHECK(std::memcmp(hello, kHelloData.data(), kHelloData.size()) == 0);
    CHECK_EQ(archive.getResSize(hello), static_cast<s32>(kHelloData.size()));

    void* inner = archive.getResource("/dir/inner.bin");
    REQUIRE(inner != nullptr);
    CHECK(std::memcmp(inner, kInnerData.data(), kInnerData.size()) == 0);

    // Type-based lookup ('TEST' is the 4-char id of the "dir" node).
    void* byType = archive.getResource(0x54455354, "inner.bin");
    CHECK(byType == inner);

    // Name-based lookup (NULL_MAGIC searches every file entry by name).
    void* byName = archive.getResource(NULL_MAGIC, "hello.txt");
    CHECK(byName == hello);

    CHECK(archive.getResource("/missing.bin") == nullptr);
    CHECK_EQ(archive.countFile("/dir"), 1u);

    // No unmount() here: it ends in `delete this`, which is invalid for a
    // stack object. The destructor frees the BREAK_FLAG_1 blob (_6C) and the
    // host file-entry array when `archive` goes out of scope.
}

// ---------------------------------------------------------------------------
// Full mount through the compat DVD layer (raw + Yaz0 + garbage).
// ---------------------------------------------------------------------------

TEST_CASE(jkr_archive_mount_via_dvd) {
    JKRHeap* heap = ensureHeap();
    REQUIRE(heap != nullptr);
    // The ripper's streaming decode buffer comes from the system heap.
    REQUIRE(JKRHeap::sSystemHeap != nullptr);

    const std::string root = makeArchiveTree();
    Platform::Filesystem::setRootDir(root);
    compat::initDVD();  // rebuilds the FST over the synthetic tree

    // Raw RARC through JKRDvdFile + JKRDvdRipper (uncompressed path).
    JKRArchive* plain = JKRArchive::mount("/TestData/plain.arc", JKRArchive::MOUNT_MODE_MEM, heap,
                                          JKRArchive::MOUNT_DIRECTION_1);
    REQUIRE(plain != nullptr);
    CHECK(plain->mIsMounted);
    void* hello = plain->getResource("/hello.txt");
    REQUIRE(hello != nullptr);
    CHECK(std::memcmp(hello, kHelloData.data(), kHelloData.size()) == 0);
    void* inner = plain->getResource("/dir/inner.bin");
    REQUIRE(inner != nullptr);
    CHECK(std::memcmp(inner, kInnerData.data(), kInnerData.size()) == 0);

    // Same archive, Yaz0-compressed: the ripper detects the header and
    // streams the decode in 0x400-byte chunks (JKRDecompressFromDVD).
    JKRArchive* comp = JKRArchive::mount("/TestData/comp.arc", JKRArchive::MOUNT_MODE_MEM, heap,
                                         JKRArchive::MOUNT_DIRECTION_1);
    REQUIRE(comp != nullptr);
    CHECK(comp->mIsMounted);
    void* helloC = comp->getResource("/hello.txt");
    REQUIRE(helloC != nullptr);
    CHECK(std::memcmp(helloC, kHelloData.data(), kHelloData.size()) == 0);
    void* innerC = comp->getResource("/dir/inner.bin");
    REQUIRE(innerC != nullptr);
    CHECK(std::memcmp(innerC, kInnerData.data(), kInnerData.size()) == 0);

    // Re-mounting the same entry returns the already-mounted archive
    // (check_mount_already) with a bumped refcount.
    JKRArchive* again = JKRArchive::mount("/TestData/plain.arc", JKRArchive::MOUNT_MODE_MEM, heap,
                                          JKRArchive::MOUNT_DIRECTION_1);
    CHECK(again == plain);

    // Garbage must fail the mount (the RarcHost validation rejects it) —
    // returning nullptr, not a half-converted archive.
    JKRArchive* garbage = JKRArchive::mount("/TestData/garbage.arc", JKRArchive::MOUNT_MODE_MEM, heap,
                                            JKRArchive::MOUNT_DIRECTION_1);
    CHECK(garbage == nullptr);

    // Missing file must fail cleanly too.
    CHECK(JKRArchive::mount("/TestData/nope.arc", JKRArchive::MOUNT_MODE_MEM, heap,
                            JKRArchive::MOUNT_DIRECTION_1) == nullptr);

    plain->unmount();  // drops the extra ref from the re-mount
    plain->unmount();
    comp->unmount();
}

// ---------------------------------------------------------------------------
// MR::mountArchive / MR::loadToMainRAM (the game-facing FileUtil entry
// points, implemented over this stack in compat/game/SceneCompat.cpp).
// ---------------------------------------------------------------------------

TEST_CASE(jkr_mr_mount_archive_and_load) {
    JKRHeap* heap = ensureHeap();
    REQUIRE(heap != nullptr);

    const std::string root = makeArchiveTree();
    Platform::Filesystem::setRootDir(root);
    compat::initDVD();

    JKRMemArchive* archive = MR::mountArchive("/TestData/comp.arc", heap);
    REQUIRE(archive != nullptr);
    CHECK(archive->mIsMounted);
    void* hello = archive->getResource("/hello.txt");
    REQUIRE(hello != nullptr);
    CHECK(std::memcmp(hello, kHelloData.data(), kHelloData.size()) == 0);
    archive->unmount();

    void* raw = MR::loadToMainRAM("/TestData/plain.arc", nullptr, heap, JKRDvdRipper::ALLOC_DIRECTION_FORWARD);
    REQUIRE(raw != nullptr);
    CHECK(std::memcmp(raw, "RARC", 4) == 0);
    JKRHeap::free(raw, heap);

    CHECK(MR::loadToMainRAM("/TestData/missing.arc", nullptr, heap, JKRDvdRipper::ALLOC_DIRECTION_FORWARD) == nullptr);
}
