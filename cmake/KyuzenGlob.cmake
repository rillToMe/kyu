# ============================================================================
# KyuzenGlob.cmake — configure-time source globbing without per-build churn.
#
# WHY NOT CONFIGURE_DEPENDS:
#   CMake's CONFIGURE_DEPENDS makes the build system re-verify every glob on
#   EVERY build by running a `VerifyGlobs.cmake` script. That script is marked
#   FORCE-dirty, so Ninja considers it out of date on every invocation and
#   re-runs CMake's glob check — which invalidates the outputs of every globbed
#   source list and cascades into a full rebuild of the whole project.
#
#   Measured effect: a no-op `ninja` rebuilt 818 of 819 build steps.
#
# WHY GLOB AT ALL:
#   The Makefile used $(wildcard ...) over explicit directories, so adding a
#   file to an already-listed directory needed no Makefile edit. Preserving that
#   convenience matters more here than it would in a small project — but not at
#   the cost of a build system that never reports "no work to do".
#
# THE TRADE-OFF, MADE EXPLICIT:
#   kyuzen_glob() resolves the pattern at CONFIGURE time. Adding or removing a
#   file in a globbed directory therefore requires a reconfigure
#   (`cmake -S . -B build`), not just a build. That is the normal CMake
#   expectation, and it is cheap: reconfiguring this project takes under two
#   seconds. Nothing is cached, so the reconfigure genuinely sees the new file.
#
#   For the KERNEL, sources are listed explicitly in cmake/KyuzenSources.cmake
#   instead (Rule 18): a stray file silently entering the kernel image is a
#   worse failure mode than an edit.
# ============================================================================

include_guard(GLOBAL)

# ---------------------------------------------------------------------------
# kyuzen_glob(<out_var> <pattern>...)
#
# Resolve one or more glob patterns NOW (at configure time) and store the sorted
# result in <out_var>. No CONFIGURE_DEPENDS, so no per-build verification.
#
# DO NOT CACHE THE RESULT.
#   An earlier version stored the file list in a CACHE INTERNAL variable and
#   skipped the glob when the cache entry already existed. That looks like a
#   harmless optimisation, but it makes the cache entry win over reality: after
#   adding a source file, a plain reconfigure keeps returning the OLD list and
#   the new file is silently ignored until someone runs `cmake --fresh`.
#   Verified: with a two-file directory, reconfigure still reported one file;
#   only --fresh picked the second one up.
#
#   That is strictly worse than the Makefile this replaces — `$(wildcard)`
#   re-expanded on every run — so the glob is simply re-evaluated each configure
#   instead. Configure-time globbing is cheap (a few ms) and only runs when a
#   CMakeLists.txt changes or the user reconfigures explicitly, which is exactly
#   when the file list should be recomputed.
# ---------------------------------------------------------------------------
function(kyuzen_glob out_var)
    file(GLOB _files ${ARGN})
    list(SORT _files)
    set(${out_var} "${_files}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# kyuzen_glob_dirs(<out_var> <dir> <ext>)
#
# One level of subdirectories under <dir>, each globbed for <ext>.
# Used for the widget toolkit's layered src/ tree.
# ---------------------------------------------------------------------------
function(kyuzen_glob_dirs out_var dir ext)
    set(_all "")

    if(IS_DIRECTORY "${dir}")
        file(GLOB _subdirs LIST_DIRECTORIES true "${dir}/*")
        list(SORT _subdirs)
        foreach(_sub IN LISTS _subdirs)
            if(IS_DIRECTORY "${_sub}")
                kyuzen_glob(_files "${_sub}/*${ext}")
                list(APPEND _all ${_files})
            endif()
        endforeach()
    endif()

    set(${out_var} "${_all}" PARENT_SCOPE)
endfunction()
