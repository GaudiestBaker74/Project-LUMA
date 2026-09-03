#pragma once
// =============================================================================
// compat/gx — TPL (Texture Palette) big-endian -> host conversion (M9.5.2).
//
// The nw4r layout engine feeds layout textures through TPL blobs fetched from
// the layout archives (`GetResource('timg', name)`). On the console a TPL is
// a big-endian file whose pointer-sized fields hold 32-bit file offsets that
// TPLBind() resolves IN PLACE with `(u32)ptr` arithmetic.
//
// Neither assumption holds on the host:
//   * fields are big-endian (u16/u32/f32 + enums stored as u32);
//   * offsets live in 4-byte fields but host pointers are 8 bytes, so the
//     native TPLPalette/TPLDescriptor/TPLHeader structs (with real pointers)
//     cannot overlay the file image — sizeof and field offsets differ.
//
// So, exactly like RarcHost for RARC archives, the blob is converted: the BE
// file is parsed with explicit offset reads and a native struct tree is built
// in one allocation. Image/CLUT payloads are NOT copied — the native structs
// point into the original blob (raw texel data needs no byte swapping; the GX
// decoders consume the tiled bytes as-is).
//
// Conversions are cached by source pointer (TPL blobs live inside mounted
// archives and ReplaceImage may be called repeatedly, e.g. by animations).
// The cache revalidates the source magic on every hit, so a recycled address
// (archive unmounted, new blob at the same address) rebuilds instead of
// returning a stale conversion.
// =============================================================================

#include <revolution/tpl.h>

namespace Platform::CompatGx {

// Converts a big-endian TPL file image to a native TPLPalette tree.
// Returns nullptr when `tplData` is not a valid TPL (bad magic/offsets).
// The returned palette (and the whole native tree) is owned by the cache and
// stays valid for the lifetime of the process.
TPLPalettePtr tplToHost(void* tplData);

} // namespace Platform::CompatGx
