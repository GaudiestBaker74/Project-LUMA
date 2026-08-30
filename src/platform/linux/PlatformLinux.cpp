// =============================================================================
// Platform::Detail — Linux (POSIX) implementations.
// This is one of only two files in the whole tree allowed to touch the OS
// directly (the other is platform/windows/PlatformWindows.cpp).
// =============================================================================

#include "platform/PlatformDetail.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__linux__)
#include <pthread.h>
#include <sys/mman.h>
#endif

namespace Platform::Detail {

bool stdoutIsTerminal() {
    return isatty(STDOUT_FILENO) != 0;
}

bool stderrIsTerminal() {
    return isatty(STDERR_FILENO) != 0;
}

void localTime(std::tm* out, const std::time_t* time) {
    if (localtime_r(time, out) == nullptr) {
        std::memset(out, 0, sizeof(*out));
    }
}

std::string userConfigDir() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return xdg;
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::string(home) + "/.config";
    }
    return {};
}

std::string userDataDir() {
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
        return xdg;
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::string(home) + "/.local/share";
    }
    return {};
}

std::string executableDir() {
#if defined(__linux__)
    char buffer[4096];
    const ssize_t n = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (n > 0) {
        buffer[n] = '\0';
        std::string path(buffer);
        const size_t slash = path.find_last_of('/');
        return slash == std::string::npos ? "." : path.substr(0, slash);
    }
#else
    // POSIX without /proc (e.g. BSD): argv[0] is unreliable; return ".".
#endif
    return ".";
}

void setThreadName(const char* name) {
#if defined(__linux__)
    // pthread_setname_np copies up to 15 bytes on Linux.
    pthread_setname_np(pthread_self(), name);
#else
    (void)name;
#endif
}

void* reserveVirtual(size_t size, const char* purpose) {
    (void)purpose;
#if defined(__linux__)
    void* base = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return base == MAP_FAILED ? nullptr : base;
#else
    (void)size;
    return nullptr;
#endif
}

void releaseVirtual(void* base, size_t size) {
#if defined(__linux__)
    if (base) {
        munmap(base, size);
    }
#else
    (void)base;
    (void)size;
#endif
}

} // namespace Platform::Detail
