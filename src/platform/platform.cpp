#include "platform/platform.h"

#include "platform/PlatformDetail.h"

#include <cstdlib>

namespace Platform {

void init(const Log::Config& logConfigIn) {
    // Logging.
    Log::Config logConfig = logConfigIn;
    logConfig.color = Platform::Detail::stdoutIsTerminal() || Platform::Detail::stderrIsTerminal();
    logConfig.fatalAborts = true;
    Log::init(logConfig);

    // Timing (first sample).
    Timing::now();

    // Filesystem root: env override or default "./assets".
    std::string assetsDir;
    if (const char* env = std::getenv("GALAXY_ASSETS_DIR"); env && *env) {
        assetsDir = env;
    } else {
        assetsDir = "assets";
    }
    Filesystem::setRootDir(assetsDir);

    PL_LOG_INFO("platform", "platform initialized");
}

void shutdown() {
    Log::shutdown();
}

} // namespace Platform
