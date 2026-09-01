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
