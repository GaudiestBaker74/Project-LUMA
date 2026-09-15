#include "platform/Log/Log.h"

#include "platform/PlatformDetail.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>

namespace Platform::Log {
namespace {

std::mutex gMutex;
Config gConfig;
bool gInitialized = false;

const char* kLevelNames[static_cast<int>(Level::Count)] = {
    "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL",
};

// ANSI color per level (only used when color is enabled).
const char* kLevelColor[static_cast<int>(Level::Count)] = {
    "\x1b[90m", // TRACE  grey
    "\x1b[36m", // DEBUG  cyan
    "\x1b[0m",  // INFO   default
    "\x1b[33m", // WARN   yellow
    "\x1b[31m", // ERROR  red
    "\x1b[1;31m", // FATAL bold red
};
const char* kColorReset = "\x1b[0m";

bool levelPasses(Level level) {
    return static_cast<int>(level) >= static_cast<int>(gConfig.minLevel);
}

FILE* sinkFor(Level level) {
    // TRACE..INFO -> stdout; WARN..FATAL -> stderr.
    return static_cast<int>(level) < static_cast<int>(Level::Warn) ? stdout : stderr;
}

void writeTimestamp(FILE* out, const std::chrono::system_clock::time_point& tp) {
    const auto secs = std::chrono::time_point_cast<std::chrono::seconds>(tp);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(tp - secs).count();
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);

    std::tm tm {};
    Platform::Detail::localTime(&tm, &t);

    std::fprintf(out, "[%04d-%02d-%02d %02d:%02d:%02d.%03d]",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(millis));
}

// --- high-frequency error flood control -------------------------------------
// A runaway error path (a per-draw budget failure that logs 1,000+ lines a
// second on the fileselect screen) costs far more than the error it reports:
// every line is a formatted console write + fflush + a file open/append/close,
// and that storm alone dragged the game to single-digit FPS (the 52,248-error
// log ran at ~1,100 lines/s). Per category, at most kMaxBurst ERROR-or-above
// lines pass in any kBurstWindow; the rest are counted and reported once as a
// summary when the window rolls over (on the next line of that category).
// FATAL is never suppressed. The caller must hold gMutex.
struct BurstTracker {
    std::chrono::steady_clock::time_point windowStart{};
    int emitted = 0;    // lines allowed through in the current window
    int suppressed = 0; // lines dropped from the current window
};
constexpr int kMaxBurst = 8;
constexpr std::chrono::seconds kBurstWindow{1};

std::unordered_map<std::string, BurstTracker>& burstTrackers() {
    static std::unordered_map<std::string, BurstTracker> trackers;
    return trackers;
}

// Emits one ready-made line to the console sink (level-routed) + the file
// sink, with timestamp — the shared tail of vlog() and the summaries above.
void emitReadyLine(Level level, const char* pCategory, const char* pMessage, bool useColor) {
    FILE* out = sinkFor(level);
    if (useColor) {
        std::fprintf(out, "%s", kLevelColor[static_cast<int>(level)]);
    }
    writeTimestamp(out, std::chrono::system_clock::now());
    std::fprintf(out, " [%-5s] [%s] %s", levelToString(level), pCategory ? pCategory : "-", pMessage);
    if (useColor) {
        std::fprintf(out, "%s", kColorReset);
    }
    std::fputc('\n', out);
    std::fflush(out);

    if (!gConfig.filePath.empty()) {
        FILE* file = std::fopen(gConfig.filePath.c_str(), "a");
        if (file) {
            writeTimestamp(file, std::chrono::system_clock::now());
            std::fprintf(file, " [%-5s] [%s] %s\n", levelToString(level), pCategory ? pCategory : "-",
                         pMessage);
            std::fclose(file);
        }
    }
}

// Allows the line through, or suppresses it (returning false). May emit the
// roll-over summary for a previously suppressed window.
bool allowBurst(const char* pCategory, bool useColor) {
    if (pCategory == nullptr) {
        return true;
    }
    BurstTracker& tracker = burstTrackers()[pCategory];
    const auto now = std::chrono::steady_clock::now();
    if (now - tracker.windowStart > kBurstWindow) {
        if (tracker.suppressed > 0) {
            char summary[160];
            std::snprintf(summary, sizeof(summary), "%d more '%s' line(s) suppressed in the last second",
                          tracker.suppressed, pCategory);
            emitReadyLine(Level::Warn, pCategory, summary, useColor);
        }
        tracker.windowStart = now;
        tracker.emitted = 0;
        tracker.suppressed = 0;
    }
    if (tracker.emitted >= kMaxBurst) {
        ++tracker.suppressed;
        return false;
    }
    ++tracker.emitted;
    return true;
}

} // namespace

const char* levelToString(Level level) {
    const int idx = static_cast<int>(level);
    return (idx >= 0 && idx < static_cast<int>(Level::Count)) ? kLevelNames[idx] : "?????";
}

Level levelFromString(const char* text) {
    if (!text) {
        return Level::Count;
    }
    // Case-insensitive compare ("info", "INFO", "Info").
    for (int i = 0; i < static_cast<int>(Level::Count); ++i) {
        const char* name = kLevelNames[i];
        size_t j = 0;
        for (; name[j] != '\0'; ++j) {
            char a = name[j];
            char b = text[j];
            if (a >= 'A' && a <= 'Z') {
                a = static_cast<char>(a + 32);
            }
            if (b >= 'A' && b <= 'Z') {
                b = static_cast<char>(b + 32);
            }
            if (a != b) {
                break;
            }
        }
        if (name[j] == '\0' && text[j] == '\0') {
            return static_cast<Level>(i);
        }
    }
    return Level::Count;
}

void init(const Config& config) {
    std::lock_guard<std::mutex> lock(gMutex);
    // File sink is opened/closed per write (no persistent handle), so
    // re-init is safe at any time.
    gConfig = config;
    gInitialized = true;
}

void shutdown() {
    std::lock_guard<std::mutex> lock(gMutex);
    gInitialized = false;
}

Config& config() {
    return gConfig;
}

void setMinLevel(Level level) {
    std::lock_guard<std::mutex> lock(gMutex);
    gConfig.minLevel = level;
}

void vlog(Level level, const char* category, const char* fmt, va_list args) {
    std::lock_guard<std::mutex> lock(gMutex);
    if (!gInitialized || !levelPasses(level)) {
        return;
    }

    // FATAL always passes; everything else ERROR-or-above is flood-controlled
    // (see allowBurst) so a broken per-frame path cannot turn the log into a
    // write storm that eats the frame budget.
    if (level >= Level::Error && level != Level::Fatal && !allowBurst(category, gConfig.color)) {
        return;
    }

    char message[2048];
    message[0] = '\0';
    if (fmt) {
        std::vsnprintf(message, sizeof(message), fmt, args);
    }

    emitReadyLine(level, category, message, gConfig.color);

    if (level == Level::Fatal && gConfig.fatalAborts) {
        std::abort();
    }
}

void log(Level level, const char* category, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog(level, category, fmt, args);
    va_end(args);
}

} // namespace Platform::Log
