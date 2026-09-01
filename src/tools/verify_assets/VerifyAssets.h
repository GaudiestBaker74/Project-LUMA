#pragma once
// =============================================================================
// M7.3: verify-assets core.
//
// Checks that a user-extracted assets tree (docs/assets.md) is complete
// enough for the game to boot and that the archive files are well-formed
// (RARC or Yaz0 container magic). Purely host-side: no game/platform code,
// only std::filesystem, so the standalone tool stays lean and the same core
// is unit-tested by the test binary.
//
// What is checked:
//  - the boot directories (top-level dirs that StationedArchiveLoader and
//    friends read at boot);
//  - every stationed archive (the 185 files mounted at boot, generated into
//    StationedManifest.h from the decomp's StationedFileInfo.cpp);
//  - optional directories used by later game systems (/MovieData, /AudioRes,
//    ...) — reported as warnings, not failures;
//  - the 4-byte header magic of every *.arc file (RARC = raw RARC archive,
//    Yaz0 = compressed container) — malformed files are warnings (rips vary).
// =============================================================================

#include <string>
#include <vector>

namespace verify_assets {

struct Report {
    bool rootOk = false;
    std::vector<std::string> missingDirs;     // fatal (boot dirs)
    std::vector<std::string> missingArchives; // fatal (stationed archives)
    std::vector<std::string> missingOptionalDirs;  // warnings
    std::vector<std::string> badArchiveHeaders;    // warnings
    size_t fileCount = 0;     // files found under the root
    size_t archiveCount = 0;  // *.arc files found
    size_t dirCount = 0;      // directories found (incl. root)

    bool ok() const { return rootOk && missingDirs.empty() && missingArchives.empty(); }
};

// Verifies the tree rooted at `root` (host path). `requiredArcPaths` lets
// tests pass a smaller manifest; defaults to the full stationed table.
Report checkTree(const std::string& root,
                 const std::vector<std::string>* requiredArcPaths = nullptr);

} // namespace verify_assets
