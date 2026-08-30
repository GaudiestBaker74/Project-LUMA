// =============================================================================
// Platform::Detail — Windows (Win32) implementations.
// This is one of only two files in the whole tree allowed to touch the OS
// directly (the other is platform/linux/PlatformLinux.cpp).
//
// NOTE: written for M2 (Windows). It is NOT compiled in the M1 (Linux) build
// and has not been compiled yet — verify on Windows in M2.
// TODO(PC_PORT): compile and verify on Windows (M2).
// =============================================================================

#include "platform/PlatformDetail.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>

#include <cstdlib>
#include <cstring>

namespace Platform::Detail {

bool stdoutIsTerminal() {
    return _isatty(_fileno(stdout)) != 0;
}

bool stderrIsTerminal() {
    return _isatty(_fileno(stderr)) != 0;
}

void localTime(std::tm* out, const std::time_t* time) {
    if (localtime_s(out, time) != 0) {
        std::memset(out, 0, sizeof(*out));
    }
}

std::string userConfigDir() {
    // Windows convention: both config and data under %APPDATA%.
    if (const char* appData = std::getenv("APPDATA"); appData && *appData) {
        return appData;
    }
    return {};
}

std::string userDataDir() {
    return userConfigDir();
}

std::string executableDir() {
    char buffer[4096];
    const DWORD n = GetModuleFileNameA(nullptr, buffer, sizeof(buffer));
    if (n == 0 || n >= sizeof(buffer)) {
        return ".";
    }
    std::string path(buffer, n);
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? "." : path.substr(0, slash);
}

void setThreadName(const char* name) {
    // SetThreadDescription is the modern API (Win10 1607+); not available on
    // older SDKs — guarded, non-fatal on failure.
    using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static SetThreadDescriptionFn fn = [] {
        HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
        if (!kernel32) {
            return static_cast<SetThreadDescriptionFn>(nullptr);
        }
        // Copy the FARPROC through memory: direct casts between function
        // pointer types are not portable (GCC -Wcast-function-type).
        FARPROC proc = GetProcAddress(kernel32, "SetThreadDescription");
        SetThreadDescriptionFn result = nullptr;
        std::memcpy(&result, &proc, sizeof(result));
        return result;
    }();
    if (fn) {
        // Convert narrow name to wide.
        wchar_t wide[64];
        const size_t len = std::strlen(name);
        MultiByteToWideChar(CP_UTF8, 0, name, static_cast<int>(len < 63 ? len : 63), wide, 64);
        wide[len < 63 ? len : 63] = L'\0';
        fn(GetCurrentThread(), wide);
    }
}

void* reserveVirtual(size_t size, const char* purpose) {
    (void)purpose;
    // Reserve + commit upfront; physical pages are still only touched on
    // access, which is what we want for a large virtual "console RAM" arena.
    return VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

void releaseVirtual(void* base, size_t size) {
    (void)size;
    if (base) {
        VirtualFree(base, 0, MEM_RELEASE);
    }
}

} // namespace Platform::Detail

#endif // _WIN32
