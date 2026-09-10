// =============================================================================
// Platform::Detail — Linux (POSIX) implementations.
// This is one of only two files in the whole tree allowed to touch the OS
// directly (the other is platform/windows/PlatformWindows.cpp).
// =============================================================================

#include "platform/PlatformDetail.h"

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__linux__)
#include <execinfo.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#endif

namespace Platform::Detail {

// --- crash reporting ---------------------------------------------------------
namespace {

// Async-signal-safe helpers: no malloc, no stdio (write(2) only).
void writeStr(int fd, const char* s) {
    if (s) {
        ssize_t r = write(fd, s, std::strlen(s));
        (void)r;
    }
}

void writeHex(int fd, unsigned long long v) {
    char buf[2 + 16 + 1];
    buf[0] = '0';
    buf[1] = 'x';
    int n = 0;
    char tmp[16];
    do {
        tmp[n++] = "0123456789abcdef"[v & 0xF];
        v >>= 4;
    } while (v != 0 && n < 16);
    int pos = 2;
    while (n > 0) {
        buf[pos++] = tmp[--n];
    }
    buf[pos] = '\0';
    writeStr(fd, buf);
}

void writeDec(int fd, unsigned long long v) {
    char tmp[24];
    int n = 0;
    do {
        tmp[n++] = static_cast<char>('0' + (v % 10));
        v /= 10;
    } while (v != 0 && n < 23);
    char buf[24];
    int pos = 0;
    while (n > 0) {
        buf[pos++] = tmp[--n];
    }
    buf[pos] = '\0';
    writeStr(fd, buf);
}

const char* signalName(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV (segmentation fault / access violation)";
    case SIGBUS: return "SIGBUS (bus error / misaligned access)";
    case SIGILL: return "SIGILL (illegal instruction)";
    case SIGFPE: return "SIGFPE (arithmetic fault)";
    case SIGABRT: return "SIGABRT (abort — OSPanic / assert / std::terminate)";
    default: return "signal";
    }
}

char gCrashLogPath[4096] = {0};
volatile sig_atomic_t gCrashHandlerInstalled = 0;
volatile sig_atomic_t gCrashInProgress = 0;

void writeCrashRecord(int fd, int sig, const siginfo_t* info) {
    writeStr(fd, "\n*** CRASH *** ");
    writeStr(fd, signalName(sig));
    writeStr(fd, "\n  fault address : ");
    writeHex(fd, info ? reinterpret_cast<unsigned long long>(info->si_addr) : 0ULL);
    writeStr(fd, "\n  si_code       : ");
    writeDec(fd, info ? static_cast<unsigned long long>(static_cast<long long>(info->si_code)) : 0ULL);
    writeStr(fd, "\n  thread (tid)  : ");
#if defined(__linux__)
    writeDec(fd, static_cast<unsigned long long>(syscall(SYS_gettid)));
    char tname[32] = {0};
    if (pthread_getname_np(pthread_self(), tname, sizeof(tname)) == 0 && tname[0] != '\0') {
        writeStr(fd, " '");
        writeStr(fd, tname);
        writeStr(fd, "'");
    }
#else
    writeDec(fd, static_cast<unsigned long long>(getpid()));
#endif
    writeStr(fd, "\n  backtrace     :\n");
#if defined(__linux__)
    void* frames[64];
    const int n = backtrace(frames, 64);
    // backtrace_symbols_fd is async-signal-safe (no malloc).
    backtrace_symbols_fd(frames, n, fd);
    writeStr(fd, "  (symbolize with: addr2line -e galaxy-pc -f -C -p <addr>)\n");
#endif
    writeStr(fd, "*** END CRASH ***\n");
}

void crashSignalHandler(int sig, siginfo_t* info, void* /*ctx*/) {
    if (gCrashInProgress) {
        // Recursive fault inside the handler — give up quietly.
        _exit(128 + sig);
    }
    gCrashInProgress = 1;

    writeCrashRecord(STDERR_FILENO, sig, info);
    if (gCrashLogPath[0] != '\0') {
        const int fd = open(gCrashLogPath, O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (fd >= 0) {
            writeCrashRecord(fd, sig, info);
            close(fd);
        }
    }

    // Re-raise with the default disposition so the exit status / core dump
    // behave as if no handler had been installed.
    signal(sig, SIG_DFL);
    raise(sig);
}

} // namespace

void writeBacktrace(FILE* out, int skipFrames) {
#if defined(__linux__)
    if (!out) {
        return;
    }
    void* frames[64];
    const int n = backtrace(frames, 64);
    std::fflush(out);
    const int skip = skipFrames < 0 ? 0 : (skipFrames > n ? n : skipFrames);
    backtrace_symbols_fd(frames + skip, n - skip, fileno(out));
#else
    (void)out;
    (void)skipFrames;
#endif
}

void installCrashHandler(const std::string& logFilePath) {
    std::strncpy(gCrashLogPath, logFilePath.c_str(), sizeof(gCrashLogPath) - 1);
    gCrashLogPath[sizeof(gCrashLogPath) - 1] = '\0';
    if (gCrashHandlerInstalled) {
        return;
    }
    gCrashHandlerInstalled = 1;

    // Dedicated stack so a stack overflow can still be reported.
    static char altStack[64 * 1024];
    stack_t ss;
    std::memset(&ss, 0, sizeof(ss));
    ss.ss_sp = altStack;
    ss.ss_size = sizeof(altStack);
    ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);

    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crashSignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
    for (int s : sigs) {
        sigaction(s, &sa, nullptr);
    }
}

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
