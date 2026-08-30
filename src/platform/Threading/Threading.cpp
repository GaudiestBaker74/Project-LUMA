#include "platform/Threading/Threading.h"

#include "platform/PlatformDetail.h"

#include <cstring>
#include <thread>

namespace Platform::Threading {

uint64_t currentThreadId() {
    // std::thread::id is a trivially copyable opaque scalar whose bytes encode
    // a per-thread token (an integer on glibc, a pointer on MinGW/MSVC). We
    // copy its bytes directly instead of streaming it: operator<< formats it
    // as decimal on glibc but hexadecimal on MinGW/MSVC, so parsing the stream
    // is not portable. The byte value is unique among running threads and
    // never zero.
    static_assert(sizeof(std::thread::id) <= sizeof(uint64_t),
                  "std::thread::id must fit in a uint64_t");
    uint64_t raw = 0;
    const std::thread::id id = std::this_thread::get_id();
    std::memcpy(&raw, &id, sizeof(id));
    return raw;
}

void setCurrentThreadName(const char* name) {
    Platform::Detail::setThreadName(name);
}

} // namespace Platform::Threading
