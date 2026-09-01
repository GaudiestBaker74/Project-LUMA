// =============================================================================
// M7.3: verify-assets core tests (headless, synthetic trees).
// =============================================================================

#include "tests/test_runner.h"

#include "tools/verify_assets/VerifyAssets.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

const std::string& root() {
    static const std::string r = (fs::temp_directory_path() / "galaxy-pc-verify").string();
    return r;
}

// Builds a tree with the six boot directories and the given .arc files
// (relative paths, e.g. "ObjectData/Mario.arc"). Every .arc gets a RARC
// header unless `badHeader` lists it (then it gets garbage).
void buildTree(const std::vector<std::string>& arcs, const std::vector<std::string>& badHeader) {
    fs::remove_all(root());
    fs::create_directories(root());
    for (const char* dir : {"/HomeButton2", "/LayoutData", "/MessageData", "/ObjectData",
                            "/ParticleData", "/StageData"}) {
        fs::create_directories(root() + dir);
    }
    for (const auto& arc : arcs) {
        const std::string path = root() + "/" + arc;
        fs::create_directories(fs::path(path).parent_path());
        std::ofstream f(path, std::ios::binary);
        const bool bad = std::find(badHeader.begin(), badHeader.end(), arc) != badHeader.end();
        f.write(bad ? "NOPE" : "RARC", 4);
        f.write("\x00\x01\x02", 3);
    }
    {
        std::ofstream f(root() + "/StageData/somefile.bcsv");
        f << "x";
    }
}

const std::vector<std::string> kManifest = {"ObjectData/Mario.arc", "LayoutData/Font.arc"};

} // namespace

TEST_CASE(verify_assets_complete_tree) {
    buildTree(kManifest, {});
    const auto r = verify_assets::checkTree(root(), &kManifest);
    CHECK(r.rootOk);
    CHECK(r.ok());
    CHECK(r.missingDirs.empty());
    CHECK(r.missingArchives.empty());
    CHECK(r.badArchiveHeaders.empty());
    CHECK_EQ(r.archiveCount, 2u);
    CHECK(r.fileCount >= 2u);
    CHECK(r.dirCount >= 6u);
}

TEST_CASE(verify_assets_missing_archive_is_fatal) {
    buildTree({"ObjectData/Mario.arc"}, {});  // LayoutData/Font.arc missing
    const auto r = verify_assets::checkTree(root(), &kManifest);
    CHECK(r.rootOk);
    CHECK(!r.ok());
    CHECK_EQ(r.missingArchives.size(), 1u);
    CHECK(r.missingArchives[0] == "LayoutData/Font.arc");
}

TEST_CASE(verify_assets_missing_boot_dir_is_fatal) {
    buildTree(kManifest, {});
    fs::remove_all(root() + "/StageData");
    const auto r = verify_assets::checkTree(root(), &kManifest);
    CHECK(!r.ok());
    const bool hasStageData =
        std::find(r.missingDirs.begin(), r.missingDirs.end(), "/StageData") != r.missingDirs.end();
    CHECK(hasStageData);
}

TEST_CASE(verify_assets_bad_root) {
    const auto r = verify_assets::checkTree(root() + "/definitely-missing", &kManifest);
    CHECK(!r.rootOk);
    CHECK(!r.ok());

    // A root that is a file, not a directory.
    {
        std::ofstream f(root() + "/StageData/notadir.txt");
        f << "x";
    }
    const auto r2 = verify_assets::checkTree(root() + "/StageData/notadir.txt", &kManifest);
    CHECK(!r2.rootOk);
}

TEST_CASE(verify_assets_bad_arc_header_is_warning_only) {
    buildTree(kManifest, {"ObjectData/Mario.arc"});
    const auto r = verify_assets::checkTree(root(), &kManifest);
    CHECK(r.ok());  // headers are warnings, not failures
    CHECK_EQ(r.badArchiveHeaders.size(), 1u);
    CHECK(r.badArchiveHeaders[0] == "ObjectData/Mario.arc");
}

TEST_CASE(verify_assets_optional_dirs_warn_but_pass) {
    buildTree(kManifest, {});
    const auto r = verify_assets::checkTree(root(), &kManifest);
    CHECK(r.ok());
    // /MovieData is optional and was not created.
    const bool hasMovie = std::find(r.missingOptionalDirs.begin(), r.missingOptionalDirs.end(),
                                    "/MovieData") != r.missingOptionalDirs.end();
    CHECK(hasMovie);
    CHECK(r.missingDirs.empty());
}
