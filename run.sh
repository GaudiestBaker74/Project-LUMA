#!/usr/bin/env bash
# =============================================================================
# run.sh — convenience launcher for the M1 demo build.
#
# Builds (incrementally) if needed and runs the galaxy-pc executable, passing
# any arguments through. The build directory does not survive workspace
# snapshots, so this script rebuilds it on demand instead of assuming it
# exists.
#
#   ./run.sh                      # M3 demo: ventana SDL3 + Vulkan + triángulo 60 Hz
#   ./run.sh --gpu-debug          # con capas de validación
#   ./run.sh --frames 120         # 120 frames y salida limpia (smoke/CI)
#   ./run.sh --help               # usage
#   ./run.sh --version
#
# The real executable lives at build/src/galaxy-pc; the tests at
# build/src/tests/galaxy-pc-tests (see ctest).
# =============================================================================
set -euo pipefail
cd "$(dirname "$0")"

BIN=build/src/galaxy-pc

if [[ ! -x "$BIN" ]]; then
    echo "[run.sh] build missing — configuring with -DPLATFORM=PC ..."
    cmake -B build -DPLATFORM=PC -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
    echo "[run.sh] building ..."
    cmake --build build
fi

exec "$BIN" "$@"
