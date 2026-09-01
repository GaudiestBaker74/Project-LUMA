// =============================================================================
// M7 (M7.1 + M7.2): DVD compat layer — the Wii disc over the VFS.
//
// Implements the high-level DVD API the game calls (<revolution/dvd.h>) on
// top of Platform::Filesystem. The "disc" is the user's extracted assets
// tree (docs/assets.md): virtual paths like /StageData/... resolve against
// the VFS root; there is no optical drive on PC.
//
// Design (decisions confirmed for M7, docs/milestones.md):
//  - FST in memory by scanning: DVDInit (or the first DVD call) walks the
//    assets tree and builds a directory table. Entrynums are table indices
//    (root = 0) and stay stable for the run; DVDFastOpen /
//    DVDConvertPathToEntrynum are O(1) lookups. Entry names are owned by
//    this layer — DVDDirEntry.name points into the FST and stays valid
//    until the next DVDInit.
//  - Reads: DVDReadPrio is synchronous (local disk is fast, and this is the
//    boot path); DVDReadAsyncPrio runs on a dedicated worker thread (the
//    SDK's "DVD interrupt thread"), with an SDK-style waiting queue ordered
//    by priority (lower = sooner) and callbacks invoked on that thread.
//  - DVDCancel marks queued commands canceled; the worker skips the read
//    and fires the callback with our canceled result (-2, matches Dolphin's
//    DVD_RESULT_CANCELED). In-flight reads complete (fast local disk).
//  - Drive state: assets mounted -> DVD_STATE_END (0, "ready"); no root ->
//    DVD_STATE_NO_DISK (4). Games check 0 == ready (JASAramStream,
//    GameSystemErrorWatcher).
//  - No byte cache in M7 (decision: deferred): the FST caches metadata and
//    repeated reads are served by the OS page cache.
//
// Not ported: the DVDLow* register layer (the game never calls it) and the
// SDK-internal __DVD* helpers. Result convention: >= 0 = bytes read / ok,
// -1 = error (missing file, out of bounds), -2 = canceled (async).
// =============================================================================

#include "compat/dvd/DVDCompat.h"

#include "platform/Filesystem/Filesystem.h"
#include "platform/Log/Log.h"
#include "platform/Threading/Threading.h"

#include <revolution/dvd.h>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

namespace fs = std::filesystem;

// Result convention (documented above).
constexpr s32 kResultCanceled = -2;

// ---------------------------------------------------------------------------
// In-memory FST (file system table) built from the assets tree.
// ---------------------------------------------------------------------------

struct FstEntry {
    s32 parent = -1;
    bool isDir = false;
    u64 size = 0;
    const char* name = nullptr;  // -> gFstNames (deque: references stay stable)
    std::string virtualPath;     // normalized, leading '/', no trailing '/'
    s32 firstChild = -1;         // DFS order => children of a dir are contiguous
};

std::vector<FstEntry> gFst;
std::deque<std::string> gFstNames;                  // stable string storage
std::unordered_map<std::string, s32> gEntryByPath;  // normalized virtual path -> entrynum
bool gFstReady = false;
std::string gScannedRoot;

// Per-open-directory iteration state (DVDDir is caller-owned POD, so the
// cursor lives here, keyed by the DVDDir*; removed on end-of-iteration or
// DVDCloseDir).
struct DirIter {
    s32 dirEntry;
    s32 nextChild;
};
std::unordered_map<DVDDir*, DirIter> gDirIter;

// The "disc id" of this title (stub — game code never calls
// DVDGetCurrentDiskID in the ported files; kept for API completeness).
DVDDiskID gDiskId = [] {
    DVDDiskID id{};
    std::memcpy(id.gameName, "RMG", 4);
    std::memcpy(id.company, "K1", 2);
    id.diskNumber = 0;
    id.gameVersion = 1;
    id.rvlMagic = 0x5D1C9EA3;
    id.gcMagic = 0xC2339D3D;
    return id;
}();

// Normalizes a game path for FST lookups: forces a leading '/', collapses
// duplicate '/' and strips trailing '/'. A ".." path simply won't match any
// FST entry (and the VFS rejects traversal at resolve time anyway).
std::string normalizePath(const char* path) {
    std::string out;
    bool lastWasSlash = false;
    for (const char* p = path ? path : ""; *p; ++p) {
        if (*p == '/') {
            if (lastWasSlash) {
                continue;
            }
            lastWasSlash = true;
        } else {
            lastWasSlash = false;
        }
        out += *p;
    }
    if (out.size() > 1 && out.back() == '/') {
        out.pop_back();
    }
    if (out.empty() || out.front() != '/') {
        out = "/" + out;
    }
    return out;
}

void buildFst() {
    gFst.clear();
    gFstNames.clear();
    gEntryByPath.clear();
    gFstReady = false;

    const std::string root = Platform::Filesystem::getRootDir();
    if (root.empty()) {
        PL_LOG_INFO("compat.dvd", "no assets root mounted — drive reports no disk");
        return;
    }
    std::error_code ec;
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
        PL_LOG_WARN("compat.dvd", "assets root '%s' is not a directory", root.c_str());
        return;
    }

    // Root entry (entrynum 0), virtual path "/".
    gFstNames.push_back("");
    gFst.push_back(FstEntry{});
    gFst[0].parent = -1;
    gFst[0].isDir = true;
    gFst[0].name = gFstNames.back().c_str();
    gFst[0].virtualPath = "/";
    gEntryByPath["/"] = 0;

    // Two-pass DFS with children sorted by name: first create ALL children
    // of a directory as consecutive entries, then recurse into each
    // subdirectory. This keeps every directory's children contiguous in the
    // table (DVDReadDir relies on that: next child == next index while the
    // parent matches) and yields deterministic, stable entrynums.
    std::function<void(s32)> visit = [&](s32 dirIdx) {
        const std::string hostDir = (gFst[dirIdx].virtualPath == "/")
                                        ? root
                                        : root + gFst[dirIdx].virtualPath;
        std::vector<fs::directory_entry> children;
        for (const auto& e : fs::directory_iterator(hostDir, ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            children.push_back(e);
        }
        std::sort(children.begin(), children.end(),
                  [](const fs::directory_entry& a, const fs::directory_entry& b) {
                      return a.path().filename().string() < b.path().filename().string();
                  });

        // Pass 1: create all children (they end up contiguous).
        std::vector<s32> childIndices;
        for (const auto& e : children) {
            const std::string name = e.path().filename().string();
            if (e.is_symlink(ec)) {  // never follow symlinks in the assets tree
                ec.clear();
                continue;
            }
            const bool isDir = e.is_directory(ec);
            ec.clear();
            const u64 size = isDir ? 0 : e.file_size(ec);
            ec.clear();

            const s32 idx = static_cast<s32>(gFst.size());
            gFstNames.push_back(name);
            FstEntry ent;
            ent.parent = dirIdx;
            ent.isDir = isDir;
            ent.size = size;
            ent.name = gFstNames.back().c_str();
            ent.virtualPath = (gFst[dirIdx].virtualPath == "/")
                                  ? "/" + name
                                  : gFst[dirIdx].virtualPath + "/" + name;
            ent.firstChild = -1;
            gFst.push_back(ent);
            gEntryByPath[ent.virtualPath] = idx;
            if (gFst[dirIdx].firstChild == -1) {
                gFst[dirIdx].firstChild = idx;
            }
            childIndices.push_back(idx);
        }

        // Pass 2: recurse into subdirectories (appends their subtrees AFTER
        // all of this dir's children, preserving contiguity).
        for (const s32 child : childIndices) {
            if (gFst[child].isDir) {
                visit(child);
            }
        }
    };
    visit(0);

    gFstReady = true;
    gScannedRoot = root;
    PL_LOG_INFO("compat.dvd", "FST built from '%s': %zu entries", root.c_str(), gFst.size());
}

// Builds the FST when needed: on first use, or when the mounted root changed.
void ensureFst() {
    if (gFstReady && gScannedRoot == Platform::Filesystem::getRootDir()) {
        return;
    }
    buildFst();
}

s32 pathToEntrynum(const char* path) {
    ensureFst();
    if (!gFstReady || path == nullptr || *path == '\0') {
        return -1;
    }
    const auto it = gEntryByPath.find(normalizePath(path));
    return it == gEntryByPath.end() ? -1 : it->second;
}

s32 entryFromFileInfo(const DVDFileInfo* info) {
    if (info == nullptr || info->cb.userData == nullptr) {
        return -1;
    }
    const s32 entry = static_cast<s32>(reinterpret_cast<uintptr_t>(info->cb.userData)) - 1;
    if (entry < 0 || entry >= static_cast<s32>(gFst.size()) || gFst[entry].isDir) {
        return -1;
    }
    return entry;
}

void fillFileInfo(DVDFileInfo* info, s32 entry) {
    std::memset(info, 0, sizeof(*info));
    // We pack entrynum+1 in cb.userData (an SDK-internal field the game
    // never touches). +1 so that 0 == "not open".
    info->cb.userData = reinterpret_cast<void*>(static_cast<uintptr_t>(entry + 1));
    info->cb.state = 0;
    info->startAddr = 0;
    info->length = static_cast<u32>(gFst[entry].size);
    info->callback = nullptr;
}

// ---------------------------------------------------------------------------
// Async read worker (the SDK's "DVD interrupt thread").
// ---------------------------------------------------------------------------

struct AsyncCommand {
    enum class Kind { Read, CheckDisk };
    Kind kind = Kind::Read;
    DVDFileInfo* info = nullptr;
    DVDCommandBlock* block = nullptr;  // for CheckDisk
    void* dst = nullptr;
    s32 length = 0;
    s32 offset = 0;
    s32 prio = 2;
    std::string virtualPath;
    DVDCallback readCallback = nullptr;
    DVDCBCallback checkCallback = nullptr;
    bool canceled = false;
};

std::mutex gQueueMutex;
std::condition_variable gQueueCv;
std::deque<AsyncCommand> gQueue;
bool gShutdown = false;
std::unique_ptr<Platform::Threading::Thread> gWorker;

// Inserts a command in priority order (stable within the same priority).
void queueInsert(AsyncCommand&& cmd) {
    std::lock_guard<std::mutex> lock(gQueueMutex);
    auto it = gQueue.begin();
    while (it != gQueue.end() && it->prio <= cmd.prio) {
        ++it;
    }
    gQueue.insert(it, std::move(cmd));
}

void workerMain() {
    for (;;) {
        AsyncCommand cmd;
        {
            std::unique_lock<std::mutex> lock(gQueueMutex);
            gQueueCv.wait(lock, [] { return gShutdown || !gQueue.empty(); });
            if (gShutdown && gQueue.empty()) {
                break;
            }
            cmd = std::move(gQueue.front());
            gQueue.pop_front();
        }

        // Execute without the queue lock: callbacks may call back into the
        // DVD API (e.g. schedule another read), which would deadlock.
        if (cmd.kind == AsyncCommand::Kind::CheckDisk) {
            const s32 result = gFstReady ? 0 : -1;
            if (cmd.checkCallback) {
                cmd.checkCallback(result, cmd.block);
            }
            continue;
        }

        s32 result = -1;
        if (cmd.canceled) {
            result = kResultCanceled;
        } else if (cmd.info != nullptr && cmd.dst != nullptr && cmd.length >= 0) {
            if (Platform::Filesystem::readFile(cmd.virtualPath, cmd.dst, cmd.length, cmd.offset)) {
                result = cmd.length;
            }
        }
        if (cmd.readCallback) {
            cmd.readCallback(result, cmd.info);
        }
    }
}

void ensureWorker() {
    if (gWorker) {
        return;
    }
    gWorker = std::make_unique<Platform::Threading::Thread>("dvd-worker", workerMain);
}

} // namespace

// ===========================================================================
// DVD API (extern "C", per the vendored dvd.h).
// ===========================================================================

extern "C" {

void DVDInit(void) {
    buildFst();
}

BOOL DVDOpen(const char* path, DVDFileInfo* fileInfo) {
    ensureFst();
    if (fileInfo == nullptr) {
        return FALSE;
    }
    const s32 entry = pathToEntrynum(path);
    if (entry < 0 || gFst[entry].isDir) {
        return FALSE;  // files only — directories open with DVDOpenDir
    }
    fillFileInfo(fileInfo, entry);
    return TRUE;
}

BOOL DVDFastOpen(s32 entryNum, DVDFileInfo* fileInfo) {
    ensureFst();
    if (fileInfo == nullptr || !gFstReady || entryNum < 0 ||
        entryNum >= static_cast<s32>(gFst.size()) || gFst[entryNum].isDir) {
        return FALSE;
    }
    fillFileInfo(fileInfo, entryNum);
    return TRUE;
}

BOOL DVDClose(DVDFileInfo* fileInfo) {
    ensureFst();
    if (fileInfo == nullptr || fileInfo->cb.userData == nullptr) {
        return FALSE;
    }
    fileInfo->cb.userData = nullptr;  // invalidate: any later read fails
    return TRUE;
}

s32 DVDReadPrio(DVDFileInfo* fileInfo, void* addr, s32 length, s32 offset, s32 prio) {
    (void)prio;
    ensureFst();
    if (!gFstReady || length < 0 || offset < 0 || (length > 0 && addr == nullptr)) {
        return -1;
    }
    const s32 entry = entryFromFileInfo(fileInfo);
    if (entry < 0) {
        return -1;
    }
    const FstEntry& f = gFst[entry];
    if (length == 0) {
        return (static_cast<u64>(offset) <= f.size) ? 0 : -1;
    }
    if (static_cast<u64>(offset) + static_cast<u64>(length) > f.size) {
        return -1;  // out of bounds — the game always reads within fileInfo.length
    }
    if (!Platform::Filesystem::readFile(f.virtualPath, addr, length, offset)) {
        return -1;
    }
    return length;
}

BOOL DVDReadAsyncPrio(DVDFileInfo* fileInfo, void* addr, s32 length, s32 offset,
                      DVDCallback callback, s32 prio) {
    ensureFst();
    if (!gFstReady || fileInfo == nullptr || callback == nullptr || length < 0 || offset < 0 ||
        (length > 0 && addr == nullptr)) {
        return FALSE;
    }
    const s32 entry = entryFromFileInfo(fileInfo);
    if (entry < 0) {
        return FALSE;
    }
    const FstEntry& f = gFst[entry];
    if (static_cast<u64>(offset) + static_cast<u64>(length) > f.size) {
        return FALSE;  // validate now so the worker never reads out of bounds
    }
    // Mirror the SDK's command block for the pending transfer, like the
    // hardware would: the callback and any DVDFileInfo inspection read these.
    fileInfo->cb.addr = addr;
    fileInfo->cb.offset = offset;
    fileInfo->cb.length = length;
    fileInfo->cb.state = 0;

    AsyncCommand cmd;
    cmd.kind = AsyncCommand::Kind::Read;
    cmd.info = fileInfo;
    cmd.dst = addr;
    cmd.length = length;
    cmd.offset = offset;
    cmd.prio = prio;
    cmd.virtualPath = f.virtualPath;
    cmd.readCallback = callback;
    ensureWorker();
    queueInsert(std::move(cmd));
    gQueueCv.notify_one();
    return TRUE;
}

s32 DVDCancel(DVDCommandBlock* block) {
    // The block is the first member of DVDFileInfo, so the owning file info
    // is at the same address.
    auto* info = reinterpret_cast<DVDFileInfo*>(block);
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(gQueueMutex);
        for (auto& cmd : gQueue) {
            if (cmd.kind == AsyncCommand::Kind::Read && cmd.info == info) {
                cmd.canceled = true;
                found = true;
            }
        }
    }
    return found ? 0 : -1;
}

s32 DVDConvertPathToEntrynum(const char* path) {
    return pathToEntrynum(path);
}

BOOL DVDGetCurrentDir(char* path, u32 maxlen) {
    if (path == nullptr || maxlen < 2) {
        return FALSE;
    }
    path[0] = '/';
    path[1] = '\0';
    return TRUE;
}

BOOL DVDOpenDir(const char* path, DVDDir* dir) {
    ensureFst();
    if (dir == nullptr) {
        return FALSE;
    }
    const s32 entry = pathToEntrynum(path);
    if (entry < 0 || !gFst[entry].isDir) {
        return FALSE;
    }
    dir->entryNum = static_cast<u32>(entry);
    dir->location = 0;
    dir->next = 0;
    gDirIter[dir] = DirIter{entry, gFst[entry].firstChild};
    return TRUE;
}

BOOL DVDReadDir(DVDDir* dir, DVDDirEntry* entry) {
    ensureFst();
    if (dir == nullptr || entry == nullptr) {
        return FALSE;
    }
    const auto it = gDirIter.find(dir);
    if (it == gDirIter.end()) {
        return FALSE;  // not an open dir (or closed)
    }
    DirIter& st = it->second;
    if (st.nextChild < 0 || st.nextChild >= static_cast<s32>(gFst.size()) ||
        gFst[st.nextChild].parent != st.dirEntry) {
        gDirIter.erase(it);  // iteration finished
        return FALSE;
    }
    const FstEntry& f = gFst[st.nextChild];
    entry->entryNum = static_cast<u32>(st.nextChild);
    entry->isDir = f.isDir ? TRUE : FALSE;
    entry->name = const_cast<char*>(f.name);
    // Children are contiguous in the FST: the next child is the following
    // index while it still belongs to the same parent.
    const s32 nxt = st.nextChild + 1;
    st.nextChild = (nxt < static_cast<s32>(gFst.size()) && gFst[nxt].parent == st.dirEntry)
                       ? nxt
                       : -1;
    return TRUE;
}

BOOL DVDCloseDir(DVDDir* dir) {
    if (dir == nullptr) {
        return FALSE;
    }
    gDirIter.erase(dir);
    return TRUE;
}

s32 DVDGetDriveStatus(void) {
    ensureFst();
    return gFstReady ? DVD_STATE_END : DVD_STATE_NO_DISK;
}

BOOL DVDCheckDiskAsync(DVDCommandBlock* block, DVDCBCallback callback) {
    ensureFst();
    if (callback == nullptr) {
        return FALSE;
    }
    AsyncCommand cmd;
    cmd.kind = AsyncCommand::Kind::CheckDisk;
    cmd.block = block;
    cmd.checkCallback = callback;
    ensureWorker();
    queueInsert(std::move(cmd));
    gQueueCv.notify_one();
    return TRUE;
}

s32 DVDGetCommandBlockStatus(const DVDCommandBlock* block) {
    (void)block;
    return 0;  // nothing is ever "in progress" from the caller's perspective
}

BOOL DVDSetAutoInvalidation(BOOL autoInvalidate) {
    (void)autoInvalidate;
    return TRUE;
}

DVDDiskID* DVDGetCurrentDiskID(void) {
    ensureFst();
    return gFstReady ? &gDiskId : nullptr;
}

BOOL DVDCompareDiskID(const DVDDiskID* id1, const DVDDiskID* id2) {
    if (id1 == nullptr || id2 == nullptr) {
        return FALSE;
    }
    return std::memcmp(id1, id2, sizeof(DVDDiskID)) == 0;
}

} // extern "C"

namespace compat {

void initDVD() {
    buildFst();
}

void shutdownDVD() {
    {
        std::lock_guard<std::mutex> lock(gQueueMutex);
        gShutdown = true;
        for (auto& cmd : gQueue) {
            cmd.canceled = true;
        }
    }
    gQueueCv.notify_all();
    if (gWorker) {
        gWorker->join();
        gWorker.reset();
    }
}

} // namespace compat
