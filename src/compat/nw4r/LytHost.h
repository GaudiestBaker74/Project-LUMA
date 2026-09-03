#pragma once
// =============================================================================
// compat/nw4r — brlyt (nw4r layout) big-endian -> host conversion (M9.5.2).
//
// The layout resources inside LayoutData/*.arc are big-endian binaries. The
// nw4r lyt engine reads them by casting struct pointers directly over the
// file image (res::BinaryFileHeader, res::Pane, res::Material, ...), which
// only works on a big-endian console. On the host every multi-byte scalar
// field is byte-swapped IN PLACE before Layout::Build sees the blob — the
// same model as compat/jsystem/RarcHost for RARC archives.
//
// Unlike RARC, no host shadow structures are needed: the lyt res structs are
// pure fixed-width scalars (no pointers), so an in-place scalar swap makes
// the file image natively readable. Strings and payloads are untouched.
//
// The conversion is idempotent: the header byteOrder mark (0xFEFF BE on
// disk) doubles as the swapped flag, so repeated calls on the same mounted
// blob (layouts are fetched per Layout::Build, archives stay mounted) are
// safe.
//
// TextBox resource strings (UTF-16BE on disk) are deliberately NOT swapped
// here — the patched lyt_textBox.cpp reads them as big-endian u16 and
// widens to the host wchar_t, because the console's 2-byte wchar_t does not
// match the host's.
// =============================================================================

#include <revolution/types.h>

namespace Platform::CompatLyt {

// Converts a big-endian brlyt image in place. `dataSize` (when non-zero) is
// cross-checked against the header's fileSize. Returns false when the blob
// is not a recognizable brlyt or its section walk runs out of bounds.
// Returns true (no-op) when the blob is already host-native.
bool convertBrlyt(void* data, u32 dataSize = 0);

// True when `data` starts with the brlyt magic ("RLYT"), in either byte
// order. Useful for callers that fetch unnamed resources from an archive.
bool isBrlyt(const void* data, u32 dataSize);

// Converts a big-endian brlan (RLAN animation) image in place (M9.5.3b).
// Walks the pai1 block down to the per-key floats: file header, block
// headers, the content/tag/entry offset tables and the hermite/step keys.
// Idempotent via the byteOrder mark, bounds-checked like convertBrlyt.
// Unknown blocks (pat1 etc.) are left untouched with a warning.
bool convertBrlan(void* data, u32 dataSize = 0);

// True when `data` starts with the brlan magic ("RLAN").
bool isBrlan(const void* data, u32 dataSize);

} // namespace Platform::CompatLyt
