#include "tests/test_runner.h"

#include "platform/Filesystem/Filesystem.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;

std::string makeTempRoot() {
    // Build-tree-local temp dir: <build>/tests_fs_root
    const std::string root = fs::temp_directory_path().string() + "/galaxy-pc-tests";
    fs::remove_all(root);
    fs::create_directories(root + "/StageData");
    fs::create_directories(root + "/ObjectData");

    {
        std::ofstream file(root + "/StageData/test.bin", std::ios::binary);
        file.write("\x01\x02\x03\x04\x05", 5);
    }
    {
        std::ofstream file(root + "/ObjectData/obj.txt");
        file << "hello";
    }
    return root;
}
} // namespace

TEST_CASE(filesystem_basic_ops) {
    const std::string root = makeTempRoot();
    Platform::Filesystem::setRootDir(root);

    CHECK(Platform::Filesystem::exists("/StageData/test.bin"));
    CHECK(Platform::Filesystem::isFile("/StageData/test.bin"));
    CHECK(Platform::Filesystem::isDirectory("/StageData"));
    CHECK(Platform::Filesystem::exists("/ObjectData"));
    CHECK(!Platform::Filesystem::exists("/Missing/file"));
    CHECK_EQ(Platform::Filesystem::fileSize("/StageData/test.bin"), static_cast<uint64_t>(5));

    // Virtual path without leading slash is tolerated.
    CHECK(Platform::Filesystem::exists("StageData/test.bin"));
}

TEST_CASE(filesystem_read) {
    const std::string root = makeTempRoot();
    Platform::Filesystem::setRootDir(root);

    uint8_t buffer[5] = {};
    CHECK(Platform::Filesystem::readFile("/StageData/test.bin", buffer, 5));
    CHECK_EQ(buffer[0], 0x01);
    CHECK_EQ(buffer[4], 0x05);

    // Partial read with offset.
    uint8_t two[2] = {};
    CHECK(Platform::Filesystem::readFile("/StageData/test.bin", two, 2, 2));
    CHECK_EQ(two[0], 0x03);
    CHECK_EQ(two[1], 0x04);

    // Whole-file read.
    const auto whole = Platform::Filesystem::readFile("/ObjectData/obj.txt");
    REQUIRE(whole.size() == 5);
    CHECK_EQ(whole[0], static_cast<uint8_t>('h'));

    // Errors.
    CHECK(!Platform::Filesystem::readFile("/DoesNotExist", buffer, 5));
    CHECK(Platform::Filesystem::readFile("/DoesNotExist").empty());
}

TEST_CASE(filesystem_list) {
    const std::string root = makeTempRoot();
    Platform::Filesystem::setRootDir(root);

    const auto entries = Platform::Filesystem::listDirectory("/StageData");
    CHECK(entries.size() == 1);
    if (entries.size() == 1) {
        CHECK_EQ(entries[0], "test.bin");
    }
}

TEST_CASE(filesystem_resolve_and_traversal) {
    const std::string root = makeTempRoot();
    Platform::Filesystem::setRootDir(root);

    // resolve() maps into the root.
    const std::string resolved = Platform::Filesystem::resolve("/StageData/test.bin");
    CHECK(!resolved.empty());
    CHECK(fs::path(resolved).is_absolute());

    // Path traversal must be rejected.
    CHECK(!Platform::Filesystem::exists("/../etc/passwd"));
    CHECK(!Platform::Filesystem::exists("/StageData/../../etc/passwd"));
    CHECK(Platform::Filesystem::resolve("/../evil").empty());

    // No root set -> everything resolves to empty / missing.
    Platform::Filesystem::setRootDir("");
    CHECK(Platform::Filesystem::resolve("/StageData/test.bin").empty());
    CHECK(!Platform::Filesystem::exists("/StageData/test.bin"));

    Platform::Filesystem::setRootDir(root); // restore for other tests
}

TEST_CASE(filesystem_host_dirs) {
    // These must return *something* sane on the host (non-empty or ".).
    const std::string config = Platform::Filesystem::configBaseDir();
    const std::string data = Platform::Filesystem::userDataBaseDir();
    const std::string exe = Platform::Filesystem::executableDir();
    CHECK(!config.empty());
    CHECK(!data.empty());
    CHECK(!exe.empty());
}
