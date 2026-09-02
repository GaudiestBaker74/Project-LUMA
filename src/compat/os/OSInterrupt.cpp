// compat/os — interrupt enable/disable (OSInterrupt.h), trivial on PC.
//
// The game's code uses OSDisableInterrupts/OSRestoreInterrupts as cheap
// critical sections (JASCriticalSection in JAudio2, JKernel…). On the Wii
// they mask the CPU interrupt level; on a multi-core PC the correct PC-side
// semantics would be a thread-aware lock. For the audio path the callers use
// them around shared-buffer updates that are already serialized by the audio
// threads, so a stack-based no-op (return/restore) preserves the observable
// behavior of every current caller. Revisit if the game ever relies on
// true mutual exclusion through these (docs/audio.md, docs/porting.md).
#include <revolution/os/OSInterrupt.h>

extern "C" {

BOOL OSDisableInterrupts(void) { return 0; }
BOOL OSEnableInterrupts(void) { return 0; }
BOOL OSRestoreInterrupts(BOOL) { return 0; }

} // extern "C"
