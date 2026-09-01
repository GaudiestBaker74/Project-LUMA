#pragma once
// =============================================================================
// M7: DVD compat layer — host-side lifecycle hooks (see DVD.cpp).
//
// The Wii disc filesystem is provided over the user's extracted assets tree
// (Platform::Filesystem VFS). This header only declares the hooks the host
// (main.cpp, the test binary) needs; the DVD API itself (<revolution/dvd.h>)
// is implemented with C linkage in DVD.cpp.
// =============================================================================

namespace compat {

// Builds the in-memory FST from the currently mounted assets root (see
// Platform::Filesystem::setRootDir). Safe to call more than once; with no
// root mounted the "drive" reports no disk and all reads fail cleanly.
void initDVD();

// Stops the async read worker and drains/cancels pending commands. Must be
// called before process exit if any async DVD read was started.
void shutdownDVD();

} // namespace compat
