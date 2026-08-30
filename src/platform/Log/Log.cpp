#include "platform/Log/Log.h"

#include "platform/PlatformDetail.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

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
    char message[2048];
    message[0] = '\0';
    if (fmt) {
        std::vsnprintf(message, sizeof(message), fmt, args);
    }

    std::lock_guard<std::mutex> lock(gMutex);
    if (!gInitialized || !levelPasses(level)) {
        return;
    }

    FILE* out = sinkFor(level);
    const bool useColor = gConfig.color;

    if (useColor) {
        std::fprintf(out, "%s", kLevelColor[static_cast<int>(level)]);
    }
    writeTimestamp(out, std::chrono::system_clock::now());
    std::fprintf(out, " [%-5s] [%s] %s", levelToString(level), category ? category : "-", message);
    if (useColor) {
        std::fprintf(out, "%s", kColorReset);
    }
    std::fputc('\n', out);
    std::fflush(out);

    if (!gConfig.filePath.empty()) {
        FILE* file = std::fopen(gConfig.filePath.c_str(), "a");
        if (file) {
            writeTimestamp(file, std::chrono::system_clock::now());
            std::fprintf(file, " [%-5s] [%s] %s\n", levelToString(level), category ? category : "-", message);
            std::fclose(file);
        }
    }

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
