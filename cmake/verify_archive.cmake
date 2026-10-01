# ============================================================================
# verify_archive.cmake — validate a static archive after it is built.
#
# Reproduces the archive sanity checks from the old Makefile (libdesktop:
# "arsip harus 5 member" + "arsip tanpa Application::run"; the SDK staging
# steps had equivalent llvm-nm greps). These catch a stale archive that was
# not rebuilt — a failure mode that is otherwise invisible until link time or
# runtime.
#
#   cmake -DARCHIVE=<path> -DKYUZEN_AR=<llvm-ar> -DKYUZEN_NM=<llvm-nm>
#         [-DEXPECTED_MEMBERS=<n>] [-DEXPECTED_SYMBOL=<mangled>]
#         -P verify_archive.cmake
# ============================================================================

if(NOT EXISTS "${ARCHIVE}")
    message(FATAL_ERROR "verify_archive: archive not found: ${ARCHIVE}")
endif()

# ---------------------------------------------------------------------------
# Member count.
# ---------------------------------------------------------------------------
if(EXPECTED_MEMBERS)
    execute_process(
        COMMAND ${KYUZEN_AR} t "${ARCHIVE}"
        OUTPUT_VARIABLE members
        ERROR_VARIABLE  ar_err
        RESULT_VARIABLE ar_rc
    )
    if(NOT ar_rc EQUAL 0)
        message(FATAL_ERROR "verify_archive: llvm-ar failed on ${ARCHIVE}: ${ar_err}")
    endif()

    string(STRIP "${members}" members)
    string(REGEX REPLACE "\r?\n" ";" member_list "${members}")
    # Trailing separators produce empty elements; drop them before counting.
    list(REMOVE_ITEM member_list "")
    list(LENGTH member_list member_count)
    if(NOT member_count EQUAL EXPECTED_MEMBERS)
        message(FATAL_ERROR
            "verify_archive: ${ARCHIVE} has ${member_count} members, "
            "expected ${EXPECTED_MEMBERS} (members: ${member_list})")
    endif()
endif()

# ---------------------------------------------------------------------------
# Required symbol.
# ---------------------------------------------------------------------------
if(EXPECTED_SYMBOL)
    execute_process(
        COMMAND ${KYUZEN_NM} --defined-only "${ARCHIVE}"
        OUTPUT_VARIABLE defined_syms
        ERROR_VARIABLE  nm_err
        RESULT_VARIABLE nm_rc
    )
    if(NOT nm_rc EQUAL 0)
        message(FATAL_ERROR "verify_archive: llvm-nm failed on ${ARCHIVE}: ${nm_err}")
    endif()

    string(FIND "${defined_syms}" "${EXPECTED_SYMBOL}" _hit)
    if(_hit EQUAL -1)
        message(FATAL_ERROR
            "verify_archive: ${ARCHIVE} does not define ${EXPECTED_SYMBOL} "
            "(stale archive?)")
    endif()
endif()
