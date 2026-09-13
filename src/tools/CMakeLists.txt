# =============================================================================
# CLI tools for the port.
# =============================================================================

# M7.3: assets-tree checker (docs/assets.md). Standalone — links nothing but
# the standard library, so it also works without a platform/Vulkan setup.
add_executable(verify-assets
    verify_assets/main.cpp
    verify_assets/VerifyAssets.cpp
)
target_include_directories(verify-assets PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
pc_enable_warnings(verify-assets)

install(TARGETS verify-assets RUNTIME DESTINATION bin)

# PC_PORT diagnostics: offline inspector for the title sky archive (prints the
# model's draw items / tex-matrix modes and dumps its textures as PPM). Links
# pc_compat for BmdModel + the BTI decoder + JKRMemArchive.
add_executable(sky-probe
    sky_probe/main.cpp
)
target_link_libraries(sky-probe PRIVATE pc_platform pc_compat)
pc_enable_warnings(sky-probe)
