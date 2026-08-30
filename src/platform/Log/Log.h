#pragma once
// =============================================================================
// Platform::Log — logging with levels TRACE..FATAL.
//
// Levels: Trace < Debug < Info < Warn < Error < Fatal.
//   Trace/Debug/Info -> stdout
//   Warn/Error       -> stderr
//   Fatal            -> stderr + abort() by default (configurable).
//
// Output format:
//   [YYYY-MM-DD HH:MM:SS.mmm] [LVL  ] [category] message
//
// Usage:
//   PL_LOG_INFO("compat.os", "arena ready: %zu bytes", size);
// =============================================================================

#include <cstdarg>
#include <cstdint>
#include <string>

namespace Platform::Log {

enum class Level : int {
    Trace = 0,
    Debug,
    Info,
    Warn,
    Error,
    Fatal,
    Count,
};

// "TRACE" / "DEBUG" / ...
const char* levelToString(Level level);
// Case-insensitive parser; returns Level::Count on unknown input.
Level levelFromString(const char* text);

struct Config {
    Level minLevel = Level::Info;
    std::string filePath; // empty = no file sink
    bool color = false;   // ANSI colors for console output
    bool fatalAborts = true;
    bool logThreadId = false;
};

// Initializes the logger (idempotent; call once). Sets up the configured
// sinks. If already initialized, re-applies the configuration.
void init(const Config& config);
void shutdown();

Config& config();
void setMinLevel(Level level);

// printf-style logging. `category` is a short subsystem tag (e.g. "compat.os").
void log(Level level, const char* category, const char* fmt, ...);
void vlog(Level level, const char* category, const char* fmt, va_list args);

} // namespace Platform::Log

// Convenience macros (the only way to forward variadic args in C/C++).
#define PL_LOG(level, category, ...) ::Platform::Log::log((level), (category), __VA_ARGS__)
#define PL_LOG_TRACE(category, ...) ::Platform::Log::log(::Platform::Log::Level::Trace, (category), __VA_ARGS__)
#define PL_LOG_DEBUG(category, ...) ::Platform::Log::log(::Platform::Log::Level::Debug, (category), __VA_ARGS__)
#define PL_LOG_INFO(category, ...) ::Platform::Log::log(::Platform::Log::Level::Info, (category), __VA_ARGS__)
#define PL_LOG_WARN(category, ...) ::Platform::Log::log(::Platform::Log::Level::Warn, (category), __VA_ARGS__)
#define PL_LOG_ERROR(category, ...) ::Platform::Log::log(::Platform::Log::Level::Error, (category), __VA_ARGS__)
#define PL_LOG_FATAL(category, ...) ::Platform::Log::log(::Platform::Log::Level::Fatal, (category), __VA_ARGS__)
