// =============================================================================
// M7 (M7.1+M7.2): DVD compat layer tests (headless).
//
// Exercises the FST (entrynums, path resolution, directory iteration) and
// the read paths (sync + async worker + cancel + drive state) against a
// synthetic assets tree in a temp directory. No GPU required.
//
// NOTE: tests run in registration order on one thread; each test mounts its
// own root and calls compat::initDVD() to rebuild the FST. The async worker
// (if started) is stopped once, by main_test.cpp after the whole suite.
//
// The compat layer stores the FST entrynum in DVDFileInfo::cb.userData, so
// tests keep their callback context in a side table keyed by DVDFileInfo*.
// =============================================================================

#include "tests/test_runner.h"

#include "compat/dvd/DVDCompat.h"

#include "platform/Filesystem/Filesystem.h"
#include "platform/Threading/Threading.h"

#include <revolution/dvd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

namespace fs = std::filesystem;

const char* kTestRoot = "galaxy-pc-dvd";  // under fs::temp_directory_path()

std::string makeTree() {
    const std::string root = (fs::temp_directory_path() / kTestRoot).string();
    fs::remove_all(root);
    fs::create_directories(root + "/StageData/Map");
    fs::create_directories(root + "/ObjectData");

    {
        std::ofstream f(root + "/StageData/WorldMap.arc", std::ios::binary);
        f.write("RARC\x00\x00\x00\x00\x01\x02\x03", 11);
    }
    {
        std::ofstream f(root + "/StageData/Map/Zone01.map", std::ios::binary);
        f.write("ZONE01", 6);
    }
    {
        std::ofstream f(root + "/ObjectData/obj.txt");
        f << "hello";  // 5 bytes
    }
    {
        std::ofstream f(root + "/Readme.txt");
        f << "readme";
    }
    return root;
}

void mount(const std::string& root) {
    Platform::Filesystem::setRootDir(root);
    compat::initDVD();  // rebuilds the FST
}

// ---------------------------------------------------------------------------
// Async-read test scaffolding.
//
// A pending read keeps a small context (result/data/thread/condvar) alive;
// the callback finds it through a side table keyed by DVDFileInfo*. The
// table is process-global and never shrinks — fine for tests.
// ---------------------------------------------------------------------------

struct ReadCtx {
    std::mutex mutex;
    std::condition_variable cv;
    bool fired = false;
    s32 result = -999;
    std::vector<uint8_t> data;
    std::thread::id workerThread;
};

std::mutex gCtxTableMutex;
std::unordered_map<DVDFileInfo*, void*> gCtxTable;

void attachCtx(DVDFileInfo* info, void* ctx) {
    std::lock_guard<std::mutex> lock(gCtxTableMutex);
    gCtxTable[info] = ctx;
}

void readCallback(s32 result, DVDFileInfo* info) {
    ReadCtx* ctx = nullptr;
    {
        std::lock_guard<std::mutex> lock(gCtxTableMutex);
        const auto it = gCtxTable.find(info);
        if (it != gCtxTable.end()) {
            ctx = static_cast<ReadCtx*>(it->second);
        }
    }
    if (ctx == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->fired = true;
        ctx->result = result;
        ctx->data.assign(static_cast<uint8_t*>(info->cb.addr),
                         static_cast<uint8_t*>(info->cb.addr) + (result > 0 ? result : 0));
        ctx->workerThread = std::this_thread::get_id();
    }
    ctx->cv.notify_all();
}

bool waitFired(ReadCtx& ctx, s32 timeoutMs = 3000) {
    std::unique_lock<std::mutex> lock(ctx.mutex);
    return ctx.cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return ctx.fired; });
}

} // namespace

TEST_CASE(dvd_fst_entrynum_resolution) {
    const std::string root = makeTree();
    mount(root);

    CHECK_EQ(DVDConvertPathToEntrynum("/"), 0);
    const s32 stageData = DVDConvertPathToEntrynum("/StageData");
    const s32 worldMap = DVDConvertPathToEntrynum("StageData/WorldMap.arc");  // no leading '/'
    CHECK(stageData > 0);
    CHECK(worldMap > 0);
    CHECK(worldMap != stageData);
    CHECK_EQ(DVDConvertPathToEntrynum("/ObjectData/obj.txt"),
             DVDConvertPathToEntrynum("ObjectData/obj.txt"));

    // Missing paths -> -1; case-sensitive like the Wii disc.
    CHECK_EQ(DVDConvertPathToEntrynum("/StageData/Missing.arc"), -1);
    CHECK_EQ(DVDConvertPathToEntrynum("/STAGEDATA/worldmap.arc"), -1);
    CHECK_EQ(DVDConvertPathToEntrynum(""), -1);

    // Redundant separators collapse (the real FS layer tolerates them).
    CHECK_EQ(DVDConvertPathToEntrynum("/StageData//WorldMap.arc"), worldMap);
    CHECK_EQ(DVDConvertPathToEntrynum("/StageData/WorldMap.arc/"), worldMap);

    // Entrynums are stable across an FST rebuild.
    compat::initDVD();
    CHECK_EQ(DVDConvertPathToEntrynum("/StageData"), stageData);
    CHECK_EQ(DVDConvertPathToEntrynum("/StageData/WorldMap.arc"), worldMap);
}

TEST_CASE(dvd_open_read_close) {
    const std::string root = makeTree();
    mount(root);

    DVDFileInfo info;
    CHECK(DVDOpen("/ObjectData/obj.txt", &info));
    CHECK_EQ(info.length, 5u);

    char buf[8] = {};
    CHECK_EQ(DVDReadPrio(&info, buf, 5, 0, 2), 5);
    CHECK(std::memcmp(buf, "hello", 5) == 0);

    // Offset reads.
    char sub[4] = {};
    CHECK_EQ(DVDReadPrio(&info, sub, 2, 1, 2), 2);
    CHECK(std::memcmp(sub, "el", 2) == 0);

    // Zero-length read at EOF is valid.
    CHECK_EQ(DVDReadPrio(&info, nullptr, 0, 5, 2), 0);
    // Reads that start inside the file but run past EOF are clamped and
    // zero-filled (console discs pad files to 32; game code reads aligned).
    CHECK_EQ(DVDReadPrio(&info, buf, 2, 4, 2), 2);
    CHECK(buf[0] == 'o');
    CHECK(buf[1] == 0);
    CHECK_EQ(DVDReadPrio(&info, buf, 1, 5, 2), 1);
    CHECK(buf[0] == 0);
    // Starting past EOF -> error.
    CHECK_EQ(DVDReadPrio(&info, buf, 1, 6, 2), -1);

    // DVDFastOpen by entrynum behaves the same.
    const s32 entry = DVDConvertPathToEntrynum("/ObjectData/obj.txt");
    DVDFileInfo fast;
    CHECK(DVDFastOpen(entry, &fast));
    CHECK_EQ(fast.length, 5u);
    std::memset(buf, 0, sizeof(buf));
    CHECK_EQ(DVDReadPrio(&fast, buf, 5, 0, 2), 5);
    CHECK(std::memcmp(buf, "hello", 5) == 0);

    // Closing invalidates the handle.
    CHECK(DVDClose(&info));
    CHECK_EQ(DVDReadPrio(&info, buf, 5, 0, 2), -1);
    CHECK(!DVDClose(&info));  // double close

    // Opening a directory as a file fails; missing file fails.
    DVDFileInfo dirInfo;
    CHECK(!DVDOpen("/StageData", &dirInfo));
    CHECK(!DVDOpen("/StageData/Nope.arc", &info));
    CHECK(!DVDFastOpen(9999, &info));
}

TEST_CASE(dvd_dir_iteration) {
    const std::string root = makeTree();
    mount(root);

    DVDDir dir;
    DVDDirEntry entry;
    CHECK(DVDOpenDir("/StageData", &dir));

    // Children of /StageData, DFS-sorted: Map (dir), WorldMap.arc (file).
    CHECK(DVDReadDir(&dir, &entry));
    CHECK(entry.isDir == TRUE);
    CHECK(std::strcmp(entry.name, "Map") == 0);
    CHECK(entry.entryNum > 0);

    CHECK(DVDReadDir(&dir, &entry));
    CHECK(entry.isDir == FALSE);
    CHECK(std::strcmp(entry.name, "WorldMap.arc") == 0);

    CHECK(!DVDReadDir(&dir, &entry));  // exhausted
    CHECK(!DVDReadDir(&dir, &entry));  // stays exhausted
    CHECK(DVDCloseDir(&dir));
    CHECK(!DVDReadDir(&dir, &entry));  // closed -> false

    // Root directory lists all top-level entries sorted.
    CHECK(DVDOpenDir("/", &dir));
    int count = 0;
    while (DVDReadDir(&dir, &entry)) {
        ++count;
    }
    CHECK_EQ(count, 3);  // ObjectData, Readme.txt, StageData
    DVDCloseDir(&dir);

    // Opening a file as a directory fails; missing dir fails.
    CHECK(!DVDOpenDir("/ObjectData/obj.txt", &dir));
    CHECK(!DVDOpenDir("/Nope", &dir));
}

TEST_CASE(dvd_async_read_worker) {
    const std::string root = makeTree();
    mount(root);

    DVDFileInfo info;
    CHECK(DVDOpen("/ObjectData/obj.txt", &info));

    ReadCtx ctx;
    ctx.data.resize(5);
    attachCtx(&info, &ctx);

    const std::thread::id mainId = std::this_thread::get_id();
    CHECK(DVDReadAsyncPrio(&info, ctx.data.data(), 5, 0, readCallback, 2));

    // The callback fires on the worker thread, not the caller's.
    CHECK(waitFired(ctx));
    {
        std::lock_guard<std::mutex> lock(ctx.mutex);
        CHECK_EQ(ctx.result, 5);
        CHECK(ctx.workerThread != mainId);
        CHECK(std::memcmp(ctx.data.data(), "hello", 5) == 0);
    }

    // Two concurrent reads on different handles both complete correctly
    // (the callback context is keyed per DVDFileInfo).
    DVDFileInfo infoA, infoB;
    CHECK(DVDOpen("/ObjectData/obj.txt", &infoA));
    CHECK(DVDOpen("/ObjectData/obj.txt", &infoB));
    ReadCtx a, b;
    char bufA[3] = {}, bufB[2] = {};
    attachCtx(&infoA, &a);
    CHECK(DVDReadAsyncPrio(&infoA, bufA, 3, 0, readCallback, 2));
    attachCtx(&infoB, &b);
    CHECK(DVDReadAsyncPrio(&infoB, bufB, 2, 3, readCallback, 2));
    CHECK(waitFired(a));
    CHECK(waitFired(b));
    {
        std::lock_guard<std::mutex> lock(a.mutex);
        CHECK_EQ(a.result, 3);
    }
    {
        std::lock_guard<std::mutex> lock(b.mutex);
        CHECK_EQ(b.result, 2);
    }
    CHECK(std::memcmp(bufA, "hel", 3) == 0);
    CHECK(std::memcmp(bufB, "lo", 2) == 0);

    // Reject invalid async reads.
    CHECK(!DVDReadAsyncPrio(&info, bufA, 5, 6, readCallback, 2));  // starts past EOF
    DVDFileInfo closed;
    CHECK(DVDOpen("/ObjectData/obj.txt", &closed));
    DVDClose(&closed);
    CHECK(!DVDReadAsyncPrio(&closed, bufA, 3, 0, readCallback, 2));  // closed handle
}

TEST_CASE(dvd_async_cancel) {
    const std::string root = makeTree();
    mount(root);

    DVDFileInfo info;
    CHECK(DVDOpen("/ObjectData/obj.txt", &info));

    // Schedule many reads so several are still queued when we cancel; the
    // worker may already have completed the first few (in-flight reads
    // finish — local disk is fast), so every callback result must be either
    // the data length (5) or canceled (-2), and at least one must be
    // canceled.
    struct CancelCtx {
        std::atomic<int> fired{0};
        std::atomic<int> canceled{0};
        std::atomic<bool> badResult{false};
    } cctx;
    attachCtx(&info, &cctx);

    constexpr int kCount = 64;
    std::vector<std::vector<char>> bufs(kCount, std::vector<char>(5, 0));
    auto cb = [](s32 r, DVDFileInfo* fi) {
        CancelCtx* ctx = nullptr;
        {
            std::lock_guard<std::mutex> lock(gCtxTableMutex);
            const auto it = gCtxTable.find(fi);
            if (it != gCtxTable.end()) {
                ctx = static_cast<CancelCtx*>(it->second);
            }
        }
        if (ctx == nullptr) {
            return;
        }
        if (r == -2) {
            ctx->canceled++;
        } else if (r != 5) {
            ctx->badResult = true;
        }
        ctx->fired++;
    };
    for (int i = 0; i < kCount; ++i) {
        if (!DVDReadAsyncPrio(&info, bufs[i].data(), 5, 0, cb, 2)) {
            CHECK(!"async schedule failed");
            break;
        }
    }
    CHECK_EQ(DVDCancel(&info.cb), 0);

    // Wait for all callbacks.
    for (int i = 0; i < 200 && cctx.fired.load() != kCount; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK_EQ(cctx.fired.load(), kCount);
    CHECK(!cctx.badResult.load());
    CHECK(cctx.canceled.load() > 0);

    // Cancel on a command that is not pending fails.
    DVDFileInfo idle;
    CHECK(DVDOpen("/ObjectData/obj.txt", &idle));
    CHECK_EQ(DVDCancel(&idle.cb), -1);
}

TEST_CASE(dvd_drive_state_and_diskid) {
    const std::string root = makeTree();
    mount(root);

    // Assets mounted -> drive ready (0 == DVD_STATE_END).
    CHECK_EQ(DVDGetDriveStatus(), 0);

    DVDDiskID* id = DVDGetCurrentDiskID();
    CHECK(id != nullptr);
    CHECK(std::memcmp(id->gameName, "RMG", 4) == 0);
    CHECK(DVDCompareDiskID(id, id) == TRUE);
    DVDDiskID other = *id;
    other.gameVersion = 2;
    CHECK(DVDCompareDiskID(id, &other) == FALSE);

    // No assets -> no disk; everything fails cleanly.
    Platform::Filesystem::setRootDir("/tmp/definitely-not-an-assets-root");
    compat::initDVD();
    CHECK_EQ(DVDGetDriveStatus(), DVD_STATE_NO_DISK);
    CHECK_EQ(DVDConvertPathToEntrynum("/"), -1);
    DVDFileInfo info;
    CHECK(!DVDOpen("/ObjectData/obj.txt", &info));
    char buf[4];
    CHECK_EQ(DVDReadPrio(&info, buf, 4, 0, 2), -1);

    // CheckDisk reports the state asynchronously.
    struct DiskCtx {
        std::mutex mutex;
        std::condition_variable cv;
        bool fired = false;
        s32 result = -999;
    } dctx;
    DVDCommandBlock block;
    std::memset(&block, 0, sizeof(block));
    block.userData = &dctx;
    CHECK(DVDCheckDiskAsync(&block, [](s32 result, DVDCommandBlock* b) {
        auto* c = static_cast<DiskCtx*>(b->userData);
        {
            std::lock_guard<std::mutex> lock(c->mutex);
            c->fired = true;
            c->result = result;
        }
        c->cv.notify_all();
    }));
    {
        std::unique_lock<std::mutex> lock(dctx.mutex);
        dctx.cv.wait_for(lock, std::chrono::seconds(3), [&] { return dctx.fired; });
        CHECK_EQ(dctx.result, -1);  // no disk
    }

    // Restore a mounted root for the rest of the suite.
    mount(root);
    CHECK_EQ(DVDGetDriveStatus(), 0);
}

TEST_CASE(dvd_current_dir_and_misc) {
    const std::string root = makeTree();
    mount(root);

    char buf[64];
    CHECK(DVDGetCurrentDir(buf, sizeof(buf)));
    CHECK(std::strcmp(buf, "/") == 0);
    CHECK(!DVDGetCurrentDir(nullptr, 4));
    CHECK(!DVDGetCurrentDir(buf, 1));

    CHECK(DVDSetAutoInvalidation(TRUE) == TRUE);
    CHECK_EQ(DVDGetCommandBlockStatus(nullptr), 0);
}
