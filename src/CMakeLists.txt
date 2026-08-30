# =============================================================================
# src — application, platform layer, Wii compatibility layer, tools, tests.
# =============================================================================

add_subdirectory(platform)
add_subdirectory(compat)
add_subdirectory(tools)
add_subdirectory(tests)

# --- Executable (M1: platform bootstrap + demo; later: the game shell) ------
add_executable(galaxy-pc main.cpp)
target_link_libraries(galaxy-pc PRIVATE pc_platform pc_compat)
pc_enable_warnings(galaxy-pc)

install(TARGETS galaxy-pc RUNTIME DESTINATION bin)
