// =============================================================================
// galaxy-pc — PC entry point (Milestone 1: platform bootstrap + demo).
//
// M1 scope: initialize the platform layer and the Wii compatibility OS,
// then run a visible smoke demo of the first native-compiled Petari module
// (the JSystem heap system). Later milestones replace this demo with the
// actual game boot (GameSystem::init + frame loop).
//
// Usage:
//   galaxy-pc [--help] [--version] [--log-level LVL] [--log-file PATH]
//             [--assets-dir DIR]
// =============================================================================

#include "platform/platform.h"

#include "compat/os/OSCompat.h"
#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr const char* kVersion = "0.1.0 (Milestone 1)";

void printHelp() {
    std::printf(
        "galaxy-pc %s\n"
        "Native PC port of Super Mario Galaxy 1 (based on Petari).\n\n"
        "Usage: galaxy-pc [options]\n"
        "  --help             show this help\n"
        "  --version          print version\n"
        "  --log-level LVL    TRACE|DEBUG|INFO|WARN|ERROR|FATAL (default INFO)\n"
        "  --log-file PATH    also write logs to PATH\n"
        "  --assets-dir DIR   game assets root (default: ./assets, env GALAXY_ASSETS_DIR)\n"
        "\n"
        "This build is a platform/demo milestone: it initializes the platform\n"
        "layer and runs the JKR heap smoke demo. No game assets are required.\n",
        kVersion);
}

// Demo: exercise the game's own memory allocator (vendored, unmodified
// Petari code) exactly the way the game boot does.
int runHeapDemo() {
    std::printf("--- JKR heap demo (Petari code running natively) ---\n");

    JKRExpHeap* root = JKRExpHeap::createRoot(1, true);
    if (!root) {
        PL_LOG_FATAL("demo", "createRoot failed");
        return 1;
    }
    std::printf("root heap: %s (%u bytes)\n",
                JKRHeap::getRootHeap() == root ? "OK" : "MISMATCH", JKRHeap::getMemorySize());

    JKRExpHeap* child = JKRExpHeap::create(0x100000, root, true);
    if (!child) {
        PL_LOG_FATAL("demo", "create child heap failed");
        return 1;
    }
    std::printf("child heap: OK (1 MiB), free initially: %d bytes\n", child->getFreeSize());

    void* a = child->alloc(4096, 16);
    void* b = new (child, 0x40) u8[256]; // placement new with alignment
    if (!a || !b) {
        PL_LOG_FATAL("demo", "allocation failed");
        return 1;
    }
    std::printf("alloc a=%p (4 KiB, align 16), b=%p (256 B, align 64)\n", a, b);

    child->free(b);
    child->free(a);
    child->freeAll();
    std::printf("after freeAll: free = %d bytes\n", child->getFreeSize());

    JKRHeap::destroy(child);
    std::printf("child heap destroyed\n");

    const auto stats = Platform::Memory::stats();
    std::printf("platform memory: %llu allocs, %llu bytes live, %llu bytes peak, arena %llu MiB\n",
                static_cast<unsigned long long>(stats.allocationCount),
                static_cast<unsigned long long>(stats.bytesAllocated),
                static_cast<unsigned long long>(stats.bytesPeak),
                static_cast<unsigned long long>(stats.arenaBytes / (1024 * 1024)));
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    // --- Parse minimal CLI ------------------------------------------------
    Platform::Log::Config logConfig;
    std::string assetsDir;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") {
            printHelp();
            return 0;
        }
        if (arg == "--version") {
            std::printf("galaxy-pc %s\n", kVersion);
            return 0;
        }
        if (arg == "--log-level" && i + 1 < argc) {
            const auto level = Platform::Log::levelFromString(argv[++i]);
            if (level == Platform::Log::Level::Count) {
                std::fprintf(stderr, "unknown log level '%s'\n", argv[i]);
                return 2;
            }
            logConfig.minLevel = level;
        } else if (arg == "--log-file" && i + 1 < argc) {
            logConfig.filePath = argv[++i];
        } else if (arg == "--assets-dir" && i + 1 < argc) {
            assetsDir = argv[++i];
        } else {
            std::fprintf(stderr, "unknown argument '%s' (see --help)\n", arg.c_str());
            return 2;
        }
    }

    // --- Platform + compat OS boot (this is where the game boot will hook) --
    Platform::init(logConfig);
    if (!assetsDir.empty()) {
        Platform::Filesystem::setRootDir(assetsDir);
    }
    compat::initOS();

    PL_LOG_INFO("main", "galaxy-pc %s — platform ready", kVersion);
    PL_LOG_INFO("main", "assets dir: '%s'", Platform::Filesystem::getRootDir().c_str());
    PL_LOG_INFO("main", "config dir: '%s'", Platform::Filesystem::configBaseDir().c_str());

    const int demoResult = runHeapDemo();

    Platform::shutdown();
    return demoResult;
}
