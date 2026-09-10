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
#include <dbghelp.h>
#include <io.h>

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#if defined(_MSC_VER)
#pragma comment(lib, "dbghelp.lib")
#endif

namespace Platform::Detail {

// --- crash reporting ---------------------------------------------------------
namespace {

char gCrashLogPath[4096] = {0};
LPTOP_LEVEL_EXCEPTION_FILTER gPreviousFilter = nullptr;
bool gCrashHandlerInstalled = false;
volatile LONG gCrashInProgress = 0;

const char* exceptionName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR: return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INVALID_OPERATION: return "EXCEPTION_FLT_INVALID_OPERATION";
    case EXCEPTION_PRIV_INSTRUCTION: return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_BREAKPOINT: return "EXCEPTION_BREAKPOINT";
    case 0xE06D7363: return "C++ exception (MSVC, uncaught)";
    case 0xC0000409: return "STATUS_STACK_BUFFER_OVERRUN (fail-fast / __fastfail / abort)";
    default: return "EXCEPTION";
    }
}

// Not strictly async-signal-safe (stdio, DbgHelp), but on Windows the filter
// runs on the faulting thread with the rest of the process suspended for
// practical purposes; this is the standard approach.
void writeBacktraceWin(FILE* out, CONTEXT* pContext, int skipFrames) {
    static std::once_flag symInit;
    const HANDLE process = GetCurrentProcess();
    std::call_once(symInit, [process] {
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
        SymInitialize(process, nullptr, TRUE);
    });

    void* frames[64];
    USHORT n = 0;
    if (pContext == nullptr) {
        n = CaptureStackBackTrace(static_cast<DWORD>(skipFrames), 64, frames, nullptr);
    } else {
        // Walk from the exception context so the first frame is the faulting
        // instruction, not the filter.
        CONTEXT ctx = *pContext;
        STACKFRAME64 sf;
        std::memset(&sf, 0, sizeof(sf));
        DWORD machine;
#if defined(_M_X64) || defined(__x86_64__)
        machine = IMAGE_FILE_MACHINE_AMD64;
        sf.AddrPC.Offset = ctx.Rip;
        sf.AddrFrame.Offset = ctx.Rbp;
        sf.AddrStack.Offset = ctx.Rsp;
#elif defined(_M_ARM64) || defined(__aarch64__)
        machine = IMAGE_FILE_MACHINE_ARM64;
        sf.AddrPC.Offset = ctx.Pc;
        sf.AddrFrame.Offset = ctx.Fp;
        sf.AddrStack.Offset = ctx.Sp;
#else
        machine = IMAGE_FILE_MACHINE_I386;
        sf.AddrPC.Offset = ctx.Eip;
        sf.AddrFrame.Offset = ctx.Ebp;
        sf.AddrStack.Offset = ctx.Esp;
#endif
        sf.AddrPC.Mode = AddrModeFlat;
        sf.AddrFrame.Mode = AddrModeFlat;
        sf.AddrStack.Mode = AddrModeFlat;
        const HANDLE thread = GetCurrentThread();
        while (n < 64 && StackWalk64(machine, process, thread, &sf, &ctx, nullptr,
                                     SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
            if (sf.AddrPC.Offset == 0) {
                break;
            }
            frames[n++] = reinterpret_cast<void*>(static_cast<uintptr_t>(sf.AddrPC.Offset));
        }
    }

    alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 256];
    for (USHORT i = 0; i < n; ++i) {
        const DWORD64 addr = reinterpret_cast<DWORD64>(frames[i]);
        SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
        std::memset(symBuf, 0, sizeof(symBuf));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        const char* name = SymFromAddr(process, addr, &disp, sym) ? sym->Name : "?";

        IMAGEHLP_LINE64 line;
        std::memset(&line, 0, sizeof(line));
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisp = 0;
        const bool hasLine = SymGetLineFromAddr64(process, addr, &lineDisp, &line) != FALSE;

        char module[MAX_PATH] = "?";
        HMODULE hMod = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(frames[i]), &hMod) && hMod) {
            GetModuleFileNameA(hMod, module, sizeof(module));
        }
        const char* modBase = std::strrchr(module, '\\');
        modBase = modBase ? modBase + 1 : module;

        if (hasLine) {
            std::fprintf(out, "    #%02u %p %s!%s+0x%llx  (%s:%lu)\n", static_cast<unsigned>(i), frames[i], modBase,
                         name, static_cast<unsigned long long>(disp), line.FileName, static_cast<unsigned long>(line.LineNumber));
        } else {
            std::fprintf(out, "    #%02u %p %s!%s+0x%llx\n", static_cast<unsigned>(i), frames[i], modBase, name,
                         static_cast<unsigned long long>(disp));
        }
    }
    std::fflush(out);
}

void writeCrashRecord(FILE* out, EXCEPTION_POINTERS* ep) {
    if (!out) {
        return;
    }
    const EXCEPTION_RECORD* rec = ep ? ep->ExceptionRecord : nullptr;
    const DWORD code = rec ? rec->ExceptionCode : 0;
    std::fprintf(out, "\n*** CRASH *** %s (0x%08lX)\n", exceptionName(code), static_cast<unsigned long>(code));
    if (rec) {
        std::fprintf(out, "  exception addr: %p\n", rec->ExceptionAddress);
        if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) {
            if (rec->NumberParameters >= 2) {
                const ULONG_PTR kind = rec->ExceptionInformation[0];
                std::fprintf(out, "  fault address : 0x%llx (%s)\n",
                             static_cast<unsigned long long>(rec->ExceptionInformation[1]),
                             kind == 0 ? "read" : kind == 1 ? "write" : kind == 8 ? "execute (DEP)" : "?");
            }
        }
    }
    std::fprintf(out, "  thread id     : %lu\n", static_cast<unsigned long>(GetCurrentThreadId()));
    std::fprintf(out, "  backtrace     :\n");
    writeBacktraceWin(out, ep ? ep->ContextRecord : nullptr, 0);
    std::fprintf(out, "*** END CRASH ***\n");
    std::fflush(out);
}

LONG WINAPI crashExceptionFilter(EXCEPTION_POINTERS* ep) {
    if (InterlockedCompareExchange(&gCrashInProgress, 1, 0) != 0) {
        // Nested fault inside the filter.
        return EXCEPTION_CONTINUE_SEARCH;
    }
    writeCrashRecord(stderr, ep);
    if (gCrashLogPath[0] != '\0') {
        if (FILE* f = std::fopen(gCrashLogPath, "a")) {
            writeCrashRecord(f, ep);
            std::fclose(f);
        }
    }
    if (gPreviousFilter) {
        return gPreviousFilter(ep);
    }
    return EXCEPTION_CONTINUE_SEARCH; // let Windows show its dialog / WER
}

// abort() (OSPanic, assert, std::terminate) does not raise an SEH exception
// that reaches the unhandled filter in release builds; hook it via signal.
void crashAbortHandler(int) {
    if (InterlockedCompareExchange(&gCrashInProgress, 1, 0) != 0) {
        return;
    }
    if (FILE* f = gCrashLogPath[0] != '\0' ? std::fopen(gCrashLogPath, "a") : nullptr) {
        std::fprintf(f, "\n*** CRASH *** SIGABRT (abort — OSPanic / assert / std::terminate)\n  thread id     : %lu\n  backtrace     :\n",
                     static_cast<unsigned long>(GetCurrentThreadId()));
        writeBacktraceWin(f, nullptr, 2);
        std::fprintf(f, "*** END CRASH ***\n");
        std::fclose(f);
    }
    std::fprintf(stderr, "\n*** CRASH *** SIGABRT (abort)\n  backtrace     :\n");
    writeBacktraceWin(stderr, nullptr, 2);
    std::fprintf(stderr, "*** END CRASH ***\n");
}

} // namespace

void writeBacktrace(FILE* out, int skipFrames) {
    if (out) {
        writeBacktraceWin(out, nullptr, skipFrames + 1);
    }
}

void installCrashHandler(const std::string& logFilePath) {
    std::strncpy(gCrashLogPath, logFilePath.c_str(), sizeof(gCrashLogPath) - 1);
    gCrashLogPath[sizeof(gCrashLogPath) - 1] = '\0';
    if (gCrashHandlerInstalled) {
        return;
    }
    gCrashHandlerInstalled = true;
    gPreviousFilter = SetUnhandledExceptionFilter(crashExceptionFilter);
    std::signal(SIGABRT, crashAbortHandler);
#if defined(_MSC_VER)
    // Keep the CRT from popping its own "abort() has been called" box before
    // we get a chance to log (the dialog is what the user saw as "the game
    // closes"); the handler above still runs.
    _set_abort_behavior(0, _WRITE_ABORT_MSG);
#endif
}

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
