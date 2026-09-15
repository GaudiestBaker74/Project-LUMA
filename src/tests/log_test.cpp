#include "tests/test_runner.h"

#include "platform/Filesystem/Filesystem.h"
#include "platform/Log/Log.h"

#include <chrono>
#include <fstream>
#include <string>
#include <thread>

namespace {

// Logs a couple of lines at different levels to a file, then reads it back
// and checks that the format/filtering behaved as expected.
std::string logPath() {
    return Platform::Filesystem::executableDir() + "/log_test_output.log";
}

int countNeedle(const std::string& path, const std::string& needle) {
    std::ifstream stream(path);
    if (!stream.good()) {
        return -1;
    }
    const std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    int n = 0;
    size_t pos = 0;
    while ((pos = content.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

} // namespace

TEST_CASE(log_level_names) {
    using Level = Platform::Log::Level;
    CHECK_EQ(std::string(Platform::Log::levelToString(Level::Trace)), "TRACE");
    CHECK_EQ(std::string(Platform::Log::levelToString(Level::Debug)), "DEBUG");
    CHECK_EQ(std::string(Platform::Log::levelToString(Level::Info)), "INFO");
    CHECK_EQ(std::string(Platform::Log::levelToString(Level::Warn)), "WARN");
    CHECK_EQ(std::string(Platform::Log::levelToString(Level::Error)), "ERROR");
    CHECK_EQ(std::string(Platform::Log::levelToString(Level::Fatal)), "FATAL");

    CHECK(Platform::Log::levelFromString("info") == Level::Info);
    CHECK(Platform::Log::levelFromString("INFO") == Level::Info);
    CHECK(Platform::Log::levelFromString("Debug") == Level::Debug);
    CHECK(Platform::Log::levelFromString("bogus") == Level::Count);
    CHECK(Platform::Log::levelFromString(nullptr) == Level::Count);
}

TEST_CASE(log_file_output_and_filtering) {
    const std::string path = logPath();
    std::remove(path.c_str());

    Platform::Log::Config config;
    config.minLevel = Platform::Log::Level::Info;
    config.filePath = path;
    config.color = false;
    Platform::Log::init(config);

    PL_LOG_TRACE("test", "trace message (filtered)");
    PL_LOG_DEBUG("test", "debug message (filtered)");
    PL_LOG_INFO("test", "info message %d", 42);
    PL_LOG_WARN("test", "warn message");
    PL_LOG_ERROR("test", "error message");

    std::ifstream stream(path);
    REQUIRE(stream.good());
    std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());

    CHECK(content.find("info message 42") != std::string::npos);
    CHECK(content.find("warn message") != std::string::npos);
    CHECK(content.find("error message") != std::string::npos);
    // Filtered below INFO.
    CHECK(content.find("trace message") == std::string::npos);
    CHECK(content.find("debug message") == std::string::npos);
    // Format sanity: [YYYY-MM-DD HH:MM:SS.mmm] [INFO ] [category] msg
    CHECK(content.find("[INFO ] [test] info message 42") != std::string::npos);
    CHECK(content.find("[WARN ] [test] warn message") != std::string::npos);

    // TRACE level re-init must let trace through.
    config.minLevel = Platform::Log::Level::Trace;
    Platform::Log::init(config);
    std::remove(path.c_str());
    PL_LOG_TRACE("test", "trace now visible");
    std::ifstream stream2(path);
    std::string content2((std::istreambuf_iterator<char>(stream2)), std::istreambuf_iterator<char>());
    CHECK(content2.find("trace now visible") != std::string::npos);
}

TEST_CASE(log_error_flood_is_rate_limited) {
    // A runaway per-frame error path (the fileselect UBO/pool exhaustion
    // logged ~1,100 lines/s) must not be able to turn the log into a write
    // storm: per category, at most kMaxBurst lines pass per second, the rest
    // are reported as one summary when the window rolls over.
    const std::string path = logPath();
    std::remove(path.c_str());

    Platform::Log::Config config;
    config.minLevel = Platform::Log::Level::Info;
    config.filePath = path;
    config.color = false;
    config.fatalAborts = false; // the test asserts on the FATAL line, not a death
    Platform::Log::init(config);

    for (int i = 0; i < 20; ++i) {
        PL_LOG_ERROR("floodtest", "flood line %d", i);
    }

    // 20 rapid errors -> only the first 8 land in the file.
    CHECK_EQ(countNeedle(path, "flood line"), 8);
    CHECK(countNeedle(path, "suppressed in the last second") == 0);

    // After the window rolls over, the next line of the category reports the
    // suppressed count and itself passes through.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    PL_LOG_ERROR("floodtest", "flood line final");

    CHECK(countNeedle(path, "12 more 'floodtest' line(s) suppressed in the last second") == 1);
    CHECK_EQ(countNeedle(path, "flood line"), 9);

    // FATAL is never suppressed, even inside a flood.
    PL_LOG_FATAL("floodtest", "fatal always lands");
    CHECK(countNeedle(path, "fatal always lands") == 1);

    Platform::Log::Config clean;
    clean.minLevel = Platform::Log::Level::Info;
    Platform::Log::init(clean);
}

TEST_CASE(log_console_does_not_crash) {
    // Console output is hard to assert on; just make sure the plumbing works.
    Platform::Log::Config config;
    config.minLevel = Platform::Log::Level::Debug;
    config.color = true; // exercise the color path
    Platform::Log::init(config);

    PL_LOG_DEBUG("test", "console debug");
    PL_LOG_INFO("test", "console info %f", 3.14);
    PL_LOG_WARN("test", "console warn");

    Platform::Log::Config clean;
    clean.minLevel = Platform::Log::Level::Info;
    Platform::Log::init(clean);
}
