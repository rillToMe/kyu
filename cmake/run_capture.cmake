# ============================================================================
# run_capture.cmake — run a tool and write its stdout to a file.
#
# Replaces a shell `>` redirect, which is not portable across the generators
# CMake may use (cmd.exe on Windows, sh elsewhere).
#
#   cmake -DTOOL=<exe> -DARG=<first-arg> -DINPUT=<input-file> -DOUTPUT=<out>
#         -P run_capture.cmake
#
# The output file is only rewritten when the content changes, so a no-op build
# does not churn the timestamp of a generated header and re-trigger everything
# that depends on it.
# ============================================================================

if(NOT TOOL OR NOT OUTPUT)
    message(FATAL_ERROR "run_capture: TOOL and OUTPUT are required")
endif()

if(NOT EXISTS "${TOOL}")
    message(FATAL_ERROR "run_capture: tool not found: ${TOOL}")
endif()

if(INPUT AND NOT EXISTS "${INPUT}")
    message(FATAL_ERROR "run_capture: input not found: ${INPUT}")
endif()

execute_process(
    COMMAND "${TOOL}" ${ARG} "${INPUT}"
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE  _stderr
    RESULT_VARIABLE _rc
)

if(NOT _rc EQUAL 0)
    message(FATAL_ERROR
        "run_capture: '${TOOL} ${ARG} ${INPUT}' failed (${_rc}):\n${_stderr}")
endif()

if(_stdout STREQUAL "")
    message(FATAL_ERROR
        "run_capture: '${TOOL} ${ARG} ${INPUT}' produced no output")
endif()

# Compare-then-write so the timestamp is stable when nothing changed.
set(_needs_write TRUE)
if(EXISTS "${OUTPUT}")
    file(READ "${OUTPUT}" _existing)
    if(_existing STREQUAL _stdout)
        set(_needs_write FALSE)
    endif()
endif()

if(_needs_write)
    get_filename_component(_dir "${OUTPUT}" DIRECTORY)
    file(MAKE_DIRECTORY "${_dir}")
    file(WRITE "${OUTPUT}" "${_stdout}")
endif()
