# ============================================================================
# verify_cpp_sdk.cmake — fail loudly on a stale C++ SDK stage.
#
# Reproduces the guards from the old Makefile's $(SDK_CPP_STAGE) recipe
# (Makefile lines ~1252-1256). Each one detects a real failure mode:
#
#   _Znwm              operator new — without it every `new` in an SDK app is
#                      an unresolved symbol
#   __cxa_guard_acquire  function-local static guards; without it any function
#                      -local static in an SDK app fails to link
#   __dso_handle       required by __cxa_atexit registration
#   __libcpp_verbose_abort  libc++'s hard-fail path (from verbose_abort.cpp)
#   _ZNSt11logic_errorC1EPKc  std::logic_error ctor (from stdexcept.cpp)
#
#   cmake -DCXXRT=<cxxrt.o> -DARCHIVE=<libcxxrt.a> -DKYUZEN_NM=<llvm-nm>
#         -DSDK_INC=<sdk cpp include> -DWRAPPER=<kyuzen-c++> -P verify_cpp_sdk.cmake
# ============================================================================

foreach(_f "${CXXRT}" "${ARCHIVE}")
    if(NOT EXISTS "${_f}")
        message(FATAL_ERROR "verify_cpp_sdk: required artifact missing: ${_f}")
    endif()
endforeach()

if(SDK_INC AND NOT IS_DIRECTORY "${SDK_INC}")
    message(FATAL_ERROR "verify_cpp_sdk: include directory missing: ${SDK_INC}")
endif()

# ---------------------------------------------------------------------------
# cxxrt.o must define the C++ runtime entry points.
# ---------------------------------------------------------------------------
execute_process(
    COMMAND ${KYUZEN_NM} --defined-only "${CXXRT}"
    OUTPUT_VARIABLE cxxrt_syms
    ERROR_VARIABLE  nm_err
    RESULT_VARIABLE nm_rc
)
if(NOT nm_rc EQUAL 0)
    message(FATAL_ERROR "verify_cpp_sdk: llvm-nm failed on ${CXXRT}: ${nm_err}")
endif()

foreach(_sym "_Znwm" "__cxa_guard_acquire" "__dso_handle")
    string(FIND "${cxxrt_syms}" "${_sym}" _hit)
    if(_hit EQUAL -1)
        message(FATAL_ERROR
            "verify_cpp_sdk: cxxrt.o does not define ${_sym} — "
            "the C++ runtime is incomplete")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# libcxxrt.a must define the libc++ subset symbols.
# ---------------------------------------------------------------------------
execute_process(
    COMMAND ${KYUZEN_NM} --defined-only "${ARCHIVE}"
    OUTPUT_VARIABLE archive_syms
    ERROR_VARIABLE  nm_err2
    RESULT_VARIABLE nm_rc2
)
if(NOT nm_rc2 EQUAL 0)
    message(FATAL_ERROR "verify_cpp_sdk: llvm-nm failed on ${ARCHIVE}: ${nm_err2}")
endif()

foreach(_sym "__libcpp_verbose_abort" "_ZNSt11logic_errorC1EPKc")
    string(FIND "${archive_syms}" "${_sym}" _hit)
    if(_hit EQUAL -1)
        message(FATAL_ERROR
            "verify_cpp_sdk: libcxxrt.a does not define ${_sym} — "
            "a stale archive (rebuild verbose_abort.cpp / stdexcept.cpp)")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Staged headers must be present, and the wrapper must be executable.
# ---------------------------------------------------------------------------
if(SDK_INC)
    foreach(_h __config_site __assertion_handler)
        if(NOT EXISTS "${SDK_INC}/${_h}")
            message(FATAL_ERROR "verify_cpp_sdk: site file '${_h}' was not staged")
        endif()
    endforeach()

    foreach(_h config.hpp app.hpp panic.hpp)
        if(NOT EXISTS "${SDK_INC}/kyuzen/${_h}")
            message(FATAL_ERROR
                "verify_cpp_sdk: public header <kyuzen/${_h}> was not staged")
        endif()
    endforeach()

    file(GLOB _desktop_headers "${SDK_INC}/kyuzen/desktop/*.hpp")
    list(LENGTH _desktop_headers _desktop_count)
    if(_desktop_count EQUAL 0)
        message(FATAL_ERROR
            "verify_cpp_sdk: no libdesktop public headers staged into "
            "<kyuzen/desktop/> (libs/gui/libdesktop/include/kyuzen/desktop/*.hpp missing?)")
    endif()
endif()

if(WRAPPER AND NOT EXISTS "${WRAPPER}")
    message(FATAL_ERROR "verify_cpp_sdk: kyuzen-c++ wrapper was not staged: ${WRAPPER}")
endif()
