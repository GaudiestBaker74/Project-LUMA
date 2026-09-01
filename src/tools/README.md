# tools

CLI utilities for the port.

- `verify-assets` (M7.3 ✅) — checks that the user-provided assets tree is
  complete and well-formed (structure/signatures only; never inspects or
  redistributes copyrighted content). Core in `verify_assets/` (shared with
  the test binary); manifest generated from the decomp's stationed-file
  table (`StationedManifest.h`). See `docs/assets.md`.
- `dump-gx` — planned: serializes the current GX state machine to JSON
  (debugging aid, tied to `compat/gx`).
