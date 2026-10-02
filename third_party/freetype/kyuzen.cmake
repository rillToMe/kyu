# ============================================================================
# third_party/freetype/kyuzen.cmake — FreeType 2.14.3, freestanding.
#
# Reproduces third_party/freetype/kyuzen.mk exactly: same 14 upstream TUs, same
# 2 Kyuzen port TUs, same flags. No upstream source is patched; the two
# FT_CONFIG_* macros redirect the library to the Kyuzen port headers.
#
# The archive lands in build/lib/libfreetype_kyuzen.a — the same path the old
# build produced, because apps/Makefile referenced it by that name.
#
# WHY NOT third_party/freetype/CMakeLists.txt: that file belongs to UPSTREAM
# FreeType (644 lines). It is not used for the freestanding target — it targets
# a hosted environment and would pull in host libc. Keeping the Kyuzen build
# integration in a separate file (mirroring the existing `kyuzen.mk` naming)
# means upstream files are never modified.
#
# NOTE: the Makefile wrote these macros as
#     -D'FT_CONFIG_STANDARD_LIBRARY_H=<ftkz_stdlib.h>'
# with single quotes so the shell would not eat the angle brackets. CMake
# passes arguments directly (no shell), so the quotes are dropped. See
# docs/development/build-system-audit.md §4.7 and risk R2.
# ============================================================================

set(_ft_dir "${KYUZEN_ROOT}/third_party/freetype")

set(_ft_sources
    # --- src/base: core amalgamation + module registration ------------------
    ${_ft_dir}/src/base/ftbase.c
    ${_ft_dir}/src/base/ftinit.c
    ${_ft_dir}/src/base/ftglyph.c
    ${_ft_dir}/src/base/ftbitmap.c
    ${_ft_dir}/src/base/ftbbox.c
    ${_ft_dir}/src/base/ftgasp.c
    ${_ft_dir}/src/base/ftmm.c          # FT_Set_Named_Instance (tt_face_init)
    # --- font drivers -------------------------------------------------------
    ${_ft_dir}/src/truetype/truetype.c  # glyph loading + interpreter
    ${_ft_dir}/src/sfnt/sfnt.c          # SFNT wrapper (needs psnames)
    # --- rasteriser + glyph names -------------------------------------------
    ${_ft_dir}/src/smooth/smooth.c      # grayscale 8-bit AA
    ${_ft_dir}/src/psnames/psnames.c
    # --- decompression ------------------------------------------------------
    ${_ft_dir}/src/gzip/ftgzip.c        # embedded zlib; WOFF/.gz via FT_Memory
    # --- Kyuzen port layer --------------------------------------------------
    ${_ft_dir}/kyuzen/src/kzf_port.c    # libc primitives
    ${_ft_dir}/kyuzen/src/kzf_system.c  # replaces ftsystem.c: no fopen/trace
)

add_library(kyuzen-freetype STATIC ${_ft_sources})
target_link_libraries(kyuzen-freetype PRIVATE kyuzen-flags-freetype)

# The library is freestanding user-space code (libtext is user-space; the
# kernel stays FreeType-free).
target_include_directories(kyuzen-freetype PUBLIC
    ${_ft_dir}/include
    ${_ft_dir}/kyuzen/include
)

set_target_properties(kyuzen-freetype PROPERTIES
    OUTPUT_NAME "freetype_kyuzen"
    ARCHIVE_OUTPUT_DIRECTORY "${KYUZEN_LIB_DIR}"
)

# Suppress warnings from upstream code we do not modify (Rule 2: no source
# rewrite). The Kyuzen port files compile clean.
target_compile_options(kyuzen-freetype PRIVATE -w)
