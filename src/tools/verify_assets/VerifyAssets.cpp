// =============================================================================
// M7.3: verify-assets core (see VerifyAssets.h).
// =============================================================================

#include "tools/verify_assets/VerifyAssets.h"

#include "tools/verify_assets/StationedManifest.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace verify_assets {

namespace fs = std::filesystem;

namespace {

// Directories StationedArchiveLoader / boot reads before anything else.
constexpr const char* kBootDirs[] = {
    "/HomeButton2", "/LayoutData", "/MessageData", "/ObjectData", "/ParticleData", "/StageData",
};

// Directories used by later systems (movies, audio, modules, ...). Their
// absence only means those features degrade — not a boot failure.
constexpr const char* kOptionalDirs[] = {
    "/MovieData", "/AudioRes", "/ModuleData", "/MapPartsData", "/DemoData",
};

bool hasArcSuffix(const std::string& name) {
    return name.size() >= 4 &&
           (name.compare(name.size() - 4, 4, ".arc") == 0 ||
            name.compare(name.size() - 4, 4, ".ARC") == 0);
}

bool isKnownContainer(const std::array<char, 4>& magic) {
    // RARC = raw RARC archive; Yaz0 = Yaz0-compressed container (the usual
    // on-disc form for SMG archives).
    const char* rarc = "RARC";
    const char* yaz0 = "Yaz0";
    return std::equal(magic.begin(), magic.end(), rarc) ||
           std::equal(magic.begin(), magic.end(), yaz0);
}

} // namespace

Report checkTree(const std::string& root, const std::vector<std::string>* requiredArcPaths) {
    Report report;

    std::error_code ec;
    if (root.empty() || !fs::exists(root, ec) || !fs::is_directory(root, ec)) {
        return report;  // rootOk stays false -> ok() == false
    }
    report.rootOk = true;

    // Full walk: count entries and sanity-check *.arc headers.
    for (const auto& entry : fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (entry.is_directory(ec)) {
            ec.clear();
            ++report.dirCount;
            continue;
        }
        ec.clear();
        ++report.fileCount;
        const std::string name = entry.path().filename().string();
        if (!hasArcSuffix(name)) {
            continue;
        }
        ++report.archiveCount;

        // Read the first 4 bytes for the container magic.
        std::ifstream stream(entry.path(), std::ios::binary);
        std::array<char, 4> magic{};
        if (stream.read(magic.data(), 4) && !isKnownContainer(magic)) {
            // Relative-ish path for the report (absolute root may vary).
            report.badArchiveHeaders.push_back(
                fs::relative(entry.path(), root, ec).generic_string());
            ec.clear();
        }
    }

    // Boot directories must exist.
    for (const char* dir : kBootDirs) {
        if (!fs::exists(root + std::string(dir), ec)) {
            report.missingDirs.push_back(dir);
        }
        ec.clear();
    }

    // Optional directories — informational only.
    for (const char* dir : kOptionalDirs) {
        if (!fs::exists(root + std::string(dir), ec)) {
            report.missingOptionalDirs.push_back(dir);
        }
        ec.clear();
    }

    // Stationed archives (mounted at boot) must all be present.
    const std::vector<std::string>* manifest = requiredArcPaths;
    if (manifest == nullptr) {
        static const std::vector<std::string> kFullManifest(kStationedArchives,
                                                            kStationedArchives +
                                                                kStationedArchiveCount);
        manifest = &kFullManifest;
    }
    for (const std::string& arcPath : *manifest) {
        // Manifest paths may be absolute ("/ObjectData/Mario.arc") or
        // relative ("ObjectData/Mario.arc") — normalize against the root.
        const std::string host =
            (!arcPath.empty() && arcPath.front() == '/') ? root + arcPath
                                                         : root + "/" + arcPath;
        if (!fs::exists(host, ec)) {
            report.missingArchives.push_back(arcPath);
        }
        ec.clear();
    }

    return report;
}

} // namespace verify_assets
