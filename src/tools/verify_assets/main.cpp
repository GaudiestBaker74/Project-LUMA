// =============================================================================
// M7.3: verify-assets — standalone assets-tree checker.
//
//   verify-assets [root] [--quiet]
//
//   root      assets directory to check (default: ./assets)
//   --quiet   only print the summary line
//
// Exit code: 0 = tree is complete and well-formed; 1 = something is missing
// or broken (see docs/assets.md).
// =============================================================================

#include "tools/verify_assets/VerifyAssets.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

void printReport(const verify_assets::Report& r, bool quiet) {
    if (quiet) {
        std::printf("%s\n", r.ok() ? "assets OK" : "assets INCOMPLETE");
        return;
    }

    if (!r.rootOk) {
        std::printf("root is not a valid assets directory\n");
        return;
    }
    std::printf("root OK: %zu dirs, %zu files (%zu archives)\n", r.dirCount, r.fileCount,
                r.archiveCount);

    if (!r.missingDirs.empty()) {
        std::printf("MISSING boot directories:\n");
        for (const auto& d : r.missingDirs) {
            std::printf("  %s\n", d.c_str());
        }
    }
    if (!r.missingArchives.empty()) {
        std::printf("MISSING stationed archives (%zu):\n", r.missingArchives.size());
        for (const auto& a : r.missingArchives) {
            std::printf("  %s\n", a.c_str());
        }
    }
    if (!r.badArchiveHeaders.empty()) {
        std::printf("WARNING — archives without a RARC/Yaz0 header (%zu):\n",
                    r.badArchiveHeaders.size());
        for (const auto& a : r.badArchiveHeaders) {
            std::printf("  %s\n", a.c_str());
        }
    }
    if (!r.missingOptionalDirs.empty()) {
        std::printf("WARNING — optional directories not found (features will degrade):\n");
        for (const auto& d : r.missingOptionalDirs) {
            std::printf("  %s\n", d.c_str());
        }
    }

    std::printf(r.ok() ? "=> assets complete: OK\n" : "=> assets INCOMPLETE (exit 1)\n");
}

} // namespace

int main(int argc, char** argv) {
    std::string root = "assets";
    bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--quiet") == 0) {
            quiet = true;
        } else if (argv[i][0] != '-') {
            root = argv[i];
        } else {
            std::fprintf(stderr, "usage: verify-assets [root] [--quiet]\n");
            return 2;
        }
    }

    const verify_assets::Report report = verify_assets::checkTree(root);
    printReport(report, quiet);
    return report.ok() ? 0 : 1;
}
