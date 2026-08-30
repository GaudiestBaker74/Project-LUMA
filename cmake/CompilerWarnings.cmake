# =============================================================================
# Compiler warning policies.
#
#   pc_enable_warnings(<target>)  — strict warnings for OUR code
#   pc_disable_warnings(<target>) — silence warnings for vendored Petari code
# =============================================================================

function(pc_enable_warnings TARGET)
    if(MSVC)
        target_compile_options(${TARGET} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${TARGET} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wnon-virtual-dtor
            -Wno-unused-parameter)
    endif()
endfunction()

function(pc_disable_warnings TARGET)
    # Vendored Petari sources are pre-C++11 decompiled code full of harmless
    # warnings (multi-char constants, long->int conversions, etc.). Silence
    # them; the game logic is validated by tests, not by -Wall.
    if(MSVC)
        target_compile_options(${TARGET} PRIVATE /w)
    else()
        target_compile_options(${TARGET} PRIVATE -w)
    endif()
endfunction()
