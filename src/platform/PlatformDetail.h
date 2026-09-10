#pragma once
// =============================================================================
// Platform::Detail — internal platform-specific hooks.
//
// THIS is the only place where platform-specific code is allowed to live
// (src/platform/linux/PlatformLinux.cpp and src/platform/windows/
// PlatformWindows.cpp). Everything else in the tree is platform-agnostic.
//
// Each function has a Linux and a Windows implementation; CMake compiles the
// right one (see cmake/PlatformSelect.cmake).
// =============================================================================

#include <cstddef>
#include <cstdio>
#include <ctime>
#include <string>

namespace Platform::Detail {

// --- console ----------------------------------------------------------------
// True if stdout/stderr are connected to an interactive terminal.
bool stdoutIsTerminal();
bool stderrIsTerminal();

// --- time -------------------------------------------------------------------
// Thread-safe local time conversion (localtime_r / localtime_s).
void localTime(std::tm* out, const std::time_t* time);

// --- directories ------------------------------------------------------------
// User config / data base directories (no trailing separator, may be empty).
//   Linux:   $XDG_CONFIG_HOME or ~/.config ; $XDG_DATA_HOME or ~/.local/share
//   Windows: %APPDATA% for both
std::string userConfigDir();
std::string userDataDir();

// Directory containing the running executable.
std::string executableDir();

// --- threads ----------------------------------------------------------------
// Best-effort naming of the calling thread (pthread_setname_np /
// SetThreadDescription).
void setThreadName(const char* name);

// --- memory -----------------------------------------------------------------
// Reserves `size` bytes of lazily-committed virtual address space
// (mmap / VirtualAlloc). Returns nullptr on failure.
void* reserveVirtual(size_t size, const char* purpose);
void releaseVirtual(void* base, size_t size);

// --- crash reporting (M9.5.4) -----------------------------------------------
// Installs a last-chance fault handler (sigaction on POSIX,
// SetUnhandledExceptionFilter on Windows). On an access violation / abort /
// illegal instruction it appends a "*** CRASH ***" record — fault kind,
// faulting address, thread id and a best-effort native backtrace — to
// `logFilePath` (may be empty: stderr only) and to stderr, then lets the
// process die normally. The game boot runs scene initialisation on worker
// threads whose faults previously killed the process without a single line
// in boot.log; this makes those crashes diagnosable from the log alone.
// Idempotent. Never throws.
void installCrashHandler(const std::string& logFilePath);

// Used by the handlers above (exposed for tests / manual dumps): appends the
// current thread's backtrace to the file descriptor / FILE*. `skipFrames`
// drops the innermost frames (the handler itself).
void writeBacktrace(FILE* out, int skipFrames);

} // namespace Platform::Detail
