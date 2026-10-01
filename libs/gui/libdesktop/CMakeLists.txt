# ============================================================================
# libs/gui/libdesktop/CMakeLists.txt — desktop framework (5 C++ sources).
#
# Static archive consumed by desktop.elf, fileman.elf, settings.elf,
# taskmgr.elf, browser.elf, and the dtsmoke probe.
#
# Compiled with the C++ SDK flag set, NOT the widget flag set: the SDK linker
# script walks .init_array, and the SDK header closure is what these sources
# include. The old Makefile used the kyuzen-c++ wrapper for these objects.
# ============================================================================

set(_libdesktop_dir "${KYUZEN_ROOT}/libs/gui/libdesktop")

set(_libdesktop_sources
    src/event.cpp
    src/window_manager.cpp
    src/canvas.cpp
    src/system.cpp
    src/application.cpp
)

add_library(kyuzen-libdesktop STATIC ${_libdesktop_sources})
target_link_libraries(kyuzen-libdesktop PRIVATE kyuzen-flags-sdk-cpp)

# Include order from the old LIBDESKTOP_SYS_INC. Note `-iquote include` rather
# than `-I include`: the repo's include/stdlib.h shim (stb_image) would
# otherwise shadow libc++'s <cstdlib>, which libc++ headers pull in via
# <new>/std::nothrow. All repo headers here are included with quotes, so
# -iquote is sufficient (BUILD_SYSTEM_AUDIT.md §13.2, risk R3).
target_include_directories(kyuzen-libdesktop PUBLIC
    ${_libdesktop_dir}/include
    ${KYUZEN_ROOT}/libs/gui/color/include
    ${KYUZEN_ROOT}/libs/text/include
)
target_compile_options(kyuzen-libdesktop PRIVATE
    -iquote ${KYUZEN_ROOT}/include
)

set_target_properties(kyuzen-libdesktop PROPERTIES
    OUTPUT_NAME "libdesktop"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/desktop"
)

# The old Makefile asserted the archive has exactly 5 members and defines
# Application::run — a guard against a stale archive silently being used.
add_custom_command(TARGET kyuzen-libdesktop POST_BUILD
    COMMAND ${CMAKE_COMMAND}
            -DARCHIVE=$<TARGET_FILE:kyuzen-libdesktop>
            -DKYUZEN_AR=${KYUZEN_LLVM_AR}
            -DKYUZEN_NM=${KYUZEN_LLVM_NM}
            -DEXPECTED_MEMBERS=5
            -DEXPECTED_SYMBOL=_ZN6kyuzen7desktop11Application3run
            -P ${KYUZEN_ROOT}/cmake/verify_archive.cmake
    COMMENT "Verifying libdesktop.a (5 members + Application::run)"
    VERBATIM
)
