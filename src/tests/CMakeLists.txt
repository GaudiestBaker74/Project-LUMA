# =============================================================================
# Unit tests (self-contained runner, see test_runner.h).
#
#   ./build/galaxy-pc-tests            run all
#   ./build/galaxy-pc-tests --list-tests
#   ./build/galaxy-pc-tests --run-test NAME
#   ctest --test-dir build
# =============================================================================

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
)
target_link_libraries(galaxy-pc-tests PRIVATE pc_platform pc_compat)
pc_enable_warnings(galaxy-pc-tests)

# The test binary initializes the platform layer and the compat OS arena
# (console RAM) before running, so the JKR heap smoke tests work.
add_test(NAME pc_unit COMMAND galaxy-pc-tests)
