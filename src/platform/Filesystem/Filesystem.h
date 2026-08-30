#pragma once
// =============================================================================
// Platform::Filesystem — virtual filesystem.
//
// The game addresses files with console-style absolute paths rooted at "/"
// (e.g. "/StageData/WorldMap.arc", "/ObjectData/..."). This module resolves
// those virtual paths against a single user-provided directory on disk
// (the extracted assets, see docs/milestones.md M7).
//
// The game code never sees host paths: it keeps using "/StageData/...".
// =============================================================================

#include <cstdint>
#include <string>
#include <vector>

namespace Platform::Filesystem {

// Sets the on-disk root directory that virtual paths resolve against.
// May be empty (no assets mounted). Paths are host-native.
void setRootDir(const std::string& path);
std::string getRootDir();

// Resolves a virtual path ("/StageData/x.arc") to a host path inside the root.
// Rejects path traversal (".."). If root is empty, returns an empty string.
// The result is NOT checked for existence.
std::string resolve(const std::string& virtualPath);

bool exists(const std::string& virtualPath);
bool isDirectory(const std::string& virtualPath);
bool isFile(const std::string& virtualPath);

// Size in bytes (0 if missing or a directory).
uint64_t fileSize(const std::string& virtualPath);

// Reads `size` bytes at `offset` into `dst`. Returns false on any error
// (missing file, short read, out of range).
bool readFile(const std::string& virtualPath, void* dst, uint64_t size, uint64_t offset = 0);

// Reads the whole file. Returns an empty vector on error.
std::vector<uint8_t> readFile(const std::string& virtualPath);

// Lists directory entries (names only, not paths). Empty on error.
std::vector<std::string> listDirectory(const std::string& virtualPath, bool recursive = false);

// --- Host-side directories (settings, saves, logs) --------------------------
// Platform-specific locations (see src/platform/{linux,windows}). App-specific
// subdirectory is appended by the caller.

// Base config directory ("~/.config" on Linux, "%APPDATA%" on Windows).
std::string configBaseDir();
// Base user-data directory ("~/.local/share" on Linux, "%APPDATA%" on Windows).
std::string userDataBaseDir();
// Directory containing the running executable.
std::string executableDir();

} // namespace Platform::Filesystem
