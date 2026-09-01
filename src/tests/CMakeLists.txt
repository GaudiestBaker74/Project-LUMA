# =============================================================================
# Unit tests (self-contained runner, see test_runner.h).
#
#   ./build/galaxy-pc-tests            run all
#   ./build/galaxy-pc-tests --list-tests
#   ./build/galaxy-pc-tests --run-test NAME
#   ctest --test-dir build
# =============================================================================

set(PETARI_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/petari)

add_executable(galaxy-pc-tests
    main_test.cpp
    test_runner.cpp
    endian_test.cpp
    memory_test.cpp
    filesystem_test.cpp
    timing_test.cpp
    threading_test.cpp
    log_test.cpp
    jkr_heap_test.cpp
    video_test.cpp
    renderer_test.cpp
    gx_test.cpp
    bti_test.cpp
    gx_tev_test.cpp
    # M6: input compat layer tests (headless: synthetic raw frames).
    input_test.cpp
    # M6.5: host shims for Petari's Metrowerks math headers (JGeometry/JMath
    # tables/intrinsics) — the prerequisite for compiling Game/System code.
    jmath_shim_test.cpp
    # M5.7c: EFB copy — state mirror, SDK formulas, GX tiled encoders (CPU)
    # + the EFB present/readback path under Xvfb.
    gx_copy_test.cpp
    # M7 (M7.1+M7.2): DVD compat — FST, sync/async reads, cancel, drive
    # state against a synthetic assets tree.
    dvd_test.cpp
    # M7.3: verify-assets core (same sources as the standalone tool).
    ../tools/verify_assets/VerifyAssets.cpp
    verify_assets_test.cpp
)
target_link_libraries(galaxy-pc-tests PRIVATE pc_platform pc_compat)
pc_enable_warnings(galaxy-pc-tests)

# Note: compiling the real vendored WPadButton.cpp (the M6 "real game code"
# integration) is deferred: its WPad.hpp → JGeometry chain needs host-compat
# shims for Petari's Metrowerks math headers (math_types.hpp std::atan2,
# JMATrigonometric.hpp std::pair, TUtil.hpp FLT_EPSILON, heapCommon.h u32
# pointer casts). That is its own milestone — needed for Game/System anyway —
# and is tracked in docs/input.md §5.3. The exact WPadButton semantics are
# pinned by the kpad_wpadbutton_semantics mirror test in input_test.cpp.

# The test binary initializes the platform layer and the compat OS arena
# (console RAM) before running, so the JKR heap smoke tests work.
add_test(NAME pc_unit COMMAND galaxy-pc-tests)
