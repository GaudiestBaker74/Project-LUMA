#include "platform/Filesystem/Filesystem.h"

#include "platform/Log/Log.h"
#include "platform/PlatformDetail.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace Platform::Filesystem {
namespace {

namespace fs = std::filesystem;

std::string gRootDir;

// Normalizes a virtual path: forces leading '/', collapses '//' and '.',
// and rejects any '..' component (path traversal must never escape the root).
bool normalizeVirtualPath(const std::string& virtualPath, std::string& out) {
    out.clear();
    if (virtualPath.empty()) {
        return false;
    }

    std::string cleaned;
    cleaned.reserve(virtualPath.size() + 1);

    // Note: no leading '/' is prepended here — the per-component loop below
    // adds one before each component, so relative and absolute inputs both
    // normalize to a single leading '/'. (A separate prepend used to produce
    // "//StageData/..." for relative paths, which escaped the VFS root.)

    size_t i = 0;
    while (i < virtualPath.size()) {
        // Skip redundant separators.
        while (i < virtualPath.size() && virtualPath[i] == '/') {
            ++i;
        }
        if (i >= virtualPath.size()) {
            break;
        }
        size_t start = i;
        while (i < virtualPath.size() && virtualPath[i] != '/') {
            ++i;
        }
        std::string component = virtualPath.substr(start, i - start);
        if (component == ".") {
            continue;
        }
        if (component == "..") {
            // Reject traversal: "/a/../b" is not allowed in the VFS.
            PL_LOG_WARN("filesystem", "path traversal rejected: '%s'", virtualPath.c_str());
            out.clear();
            return false;
        }
        cleaned += '/';
        cleaned += component;
    }

    if (cleaned.empty()) {
        cleaned = "/";
    }
    out = std::move(cleaned);
    return true;
}

fs::path toHostPath(const std::string& virtualPath) {
    std::string normalized;
    if (!normalizeVirtualPath(virtualPath, normalized) || gRootDir.empty()) {
        return {};
    }
    // Strip the leading '/' and join with the root using the native separator.
    std::string rel = normalized.substr(1);
    fs::path host = fs::path(gRootDir);
    if (!rel.empty()) {
        host /= rel;
    }
    return host;
}

} // namespace

void setRootDir(const std::string& path) {
    gRootDir = path;
    PL_LOG_INFO("filesystem", "root dir set to '%s'", gRootDir.empty() ? "(none)" : gRootDir.c_str());
}

std::string getRootDir() {
    return gRootDir;
}

std::string resolve(const std::string& virtualPath) {
    fs::path host = toHostPath(virtualPath);
    return host.empty() ? std::string() : host.string();
}

bool exists(const std::string& virtualPath) {
    fs::path host = toHostPath(virtualPath);
    return !host.empty() && fs::exists(host);
}

bool isDirectory(const std::string& virtualPath) {
    fs::path host = toHostPath(virtualPath);
    std::error_code ec;
    return !host.empty() && fs::is_directory(host, ec);
}

bool isFile(const std::string& virtualPath) {
    fs::path host = toHostPath(virtualPath);
    std::error_code ec;
    return !host.empty() && fs::is_regular_file(host, ec);
}

uint64_t fileSize(const std::string& virtualPath) {
    fs::path host = toHostPath(virtualPath);
    std::error_code ec;
    const auto size = fs::file_size(host, ec);
    return ec ? 0 : size;
}

bool readFile(const std::string& virtualPath, void* dst, uint64_t size, uint64_t offset) {
    if (!dst || size == 0) {
        return false;
    }
    fs::path host = toHostPath(virtualPath);
    if (host.empty()) {
        return false;
    }
    FILE* file = std::fopen(host.string().c_str(), "rb");
    if (!file) {
        return false;
    }
    if (offset > 0 && std::fseek(file, static_cast<long>(offset), SEEK_SET) != 0) {
        std::fclose(file);
        return false;
    }
    const size_t read = std::fread(dst, 1, size, file);
    std::fclose(file);
    return read == size;
}

std::vector<uint8_t> readFile(const std::string& virtualPath) {
    std::vector<uint8_t> data;
    fs::path host = toHostPath(virtualPath);
    if (host.empty()) {
        return data;
    }
    std::ifstream stream(host, std::ios::binary);
    if (!stream) {
        return data;
    }
    stream.seekg(0, std::ios::end);
    const auto length = stream.tellg();
    if (length <= 0) {
        return data;
    }
    stream.seekg(0, std::ios::beg);
    data.resize(static_cast<size_t>(length));
    stream.read(reinterpret_cast<char*>(data.data()), length);
    if (!stream) {
        data.clear();
        return data;
    }
    return data;
}

std::vector<std::string> listDirectory(const std::string& virtualPath, bool recursive) {
    std::vector<std::string> entries;
    fs::path host = toHostPath(virtualPath);
    if (host.empty()) {
        return entries;
    }
    std::error_code ec;
    if (recursive) {
        for (const auto& entry : fs::recursive_directory_iterator(host, fs::directory_options::skip_permission_denied, ec)) {
            entries.push_back(entry.path().filename().string());
        }
    } else {
        for (const auto& entry : fs::directory_iterator(host, fs::directory_options::skip_permission_denied, ec)) {
            entries.push_back(entry.path().filename().string());
        }
    }
    return entries;
}

std::string configBaseDir() {
    return Platform::Detail::userConfigDir();
}

std::string userDataBaseDir() {
    return Platform::Detail::userDataDir();
}

std::string executableDir() {
    return Platform::Detail::executableDir();
}

} // namespace Platform::Filesystem
