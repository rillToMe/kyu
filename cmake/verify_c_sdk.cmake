# ============================================================================
# verify_c_sdk.cmake — fail loudly on a stale C SDK stage.
#
# Reproduces the guards from the old Makefile's $(SDK_STAGE) recipe
# (Makefile lines ~1710-1715). They exist because a stale libc.a is silent:
# the build succeeds, and the app fails later with
#   "use of undeclared identifier 'malloc'"  or  "unknown type name 'ldiv_t'"
# because the SDK headers and the archive came from different libc revisions.
#
#   cmake -DSDK_LIB=<libc.a> -DSDK_INC=<include dir> -DKYUZEN_NM=<llvm-nm>
#         -P verify_c_sdk.cmake
# ============================================================================

if(NOT EXISTS "${SDK_LIB}")
    message(FATAL_ERROR "verify_c_sdk: SDK archive missing: ${SDK_LIB}")
endif()
if(NOT IS_DIRECTORY "${SDK_INC}")
    message(FATAL_ERROR "verify_c_sdk: SDK include directory missing: ${SDK_INC}")
endif()

# ---------------------------------------------------------------------------
# Required headers. Without these the SDK is unusable and the error surfaces
# much later, in an app compile.
# ---------------------------------------------------------------------------
foreach(_h stdio.h stdlib.h string.h ctype.h errno.h time.h)
    if(NOT EXISTS "${SDK_INC}/${_h}")
        message(FATAL_ERROR
            "verify_c_sdk: header '${_h}' missing from ${SDK_INC}.\n"
            "A libc.a older than Phase 4 will not have time.h — delete "
            "${KYUZEN_LIBC_DIR} and rebuild.")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Required symbols. Each one corresponds to a libc bring-up phase; a missing
# symbol means the archive is from before that phase.
# ---------------------------------------------------------------------------
execute_process(
    COMMAND ${KYUZEN_NM} --defined-only "${SDK_LIB}"
    OUTPUT_VARIABLE defined_syms
    ERROR_VARIABLE  nm_err
    RESULT_VARIABLE nm_rc
)
if(NOT nm_rc EQUAL 0)
    message(FATAL_ERROR "verify_c_sdk: llvm-nm failed on ${SDK_LIB}: ${nm_err}")
endif()

# (symbol, phase-that-added-it)
set(_required
    "printf:Phase 2 (stdio)"
    "malloc:Phase 1 (heap)"
    "qsort:Phase 4 (utils)"
    "timespec_get:Phase 4 (time)"
)

foreach(_pair IN LISTS _required)
    string(REPLACE ":" ";" _parts "${_pair}")
    list(GET _parts 0 _sym)
    list(GET _parts 1 _phase)

    # Match a definition of the symbol: a type letter then the name at end of
    # line (T/t/W/w = text/weak).
    string(REGEX MATCH "[TtWw] ${_sym}(\r?\n|$)" _hit "${defined_syms}")
    if(NOT _hit)
        message(FATAL_ERROR
            "verify_c_sdk: ${SDK_LIB} does not define '${_sym}'.\n"
            "The archive looks stale (missing ${_phase}). Delete "
            "${KYUZEN_LIBC_DIR} and rebuild.")
    endif()
endforeach()
