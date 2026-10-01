# ============================================================================
# verify_user_elf.cmake — post-build validation of a user-space ELF.
#
# Reproduces the sanity greps the old Makefile ran after linking every SDK-built
# ELF (root Makefile lines ~1572-1585, 2080-2086, 2128-2134, 2175-2181,
# 2288-2294). Those checks exist because these failures are silent at build
# time and only show up as a crash or a hang at boot.
#
# Invoked with:
#   -DELF=<path> -DKYUZEN_NM=<llvm-nm> -DKYUZEN_OBJDUMP=<llvm-objdump>
#   [-DEXPECTED_ENTRY=_start|main] [-DCHECK_NO_FP=ON]
#
# Written as a CMake script (not a shell pipeline) so it runs on any host
# without grep/sed/bash.
# ============================================================================

if(NOT EXISTS "${ELF}")
    message(FATAL_ERROR "verify_user_elf: ELF not found: ${ELF}")
endif()

# ---------------------------------------------------------------------------
# Full symbol table (used by checks 1 and 3).
# ---------------------------------------------------------------------------
execute_process(
    COMMAND ${KYUZEN_NM} "${ELF}"
    OUTPUT_VARIABLE nm_all
    ERROR_VARIABLE  nm_err
    RESULT_VARIABLE nm_rc
)
if(NOT nm_rc EQUAL 0)
    message(FATAL_ERROR "verify_user_elf: llvm-nm failed on ${ELF}: ${nm_err}")
endif()

# ---------------------------------------------------------------------------
# 1. Entry point.
# ---------------------------------------------------------------------------
if(EXPECTED_ENTRY)
    string(REGEX MATCH "[ \t][Tt] ${EXPECTED_ENTRY}(\r?\n|$)" entry_hit "${nm_all}")
    if(NOT entry_hit)
        message(FATAL_ERROR
            "verify_user_elf: entry symbol '${EXPECTED_ENTRY}' not found in ${ELF}")
    endif()
endif()

# ---------------------------------------------------------------------------
# 2. No unresolved symbols.
#
# `llvm-nm --undefined-only` lists symbols this object references but does not
# define. For a statically linked freestanding ELF the list must be empty;
# anything left is a missing archive member or a wrong link order.
# ---------------------------------------------------------------------------
execute_process(
    COMMAND ${KYUZEN_NM} --undefined-only "${ELF}"
    OUTPUT_VARIABLE nm_undef
    ERROR_VARIABLE  nm_undef_err
    RESULT_VARIABLE nm_undef_rc
)
if(NOT nm_undef_rc EQUAL 0)
    message(FATAL_ERROR "verify_user_elf: llvm-nm --undefined-only failed: ${nm_undef_err}")
endif()
string(STRIP "${nm_undef}" nm_undef_stripped)
if(NOT nm_undef_stripped STREQUAL "")
    message(FATAL_ERROR
        "verify_user_elf: ${ELF} has unresolved symbols:\n${nm_undef}")
endif()

# ---------------------------------------------------------------------------
# 3. No exception / unwinding / thread runtime.
#
# These apps have no C++ runtime: -fno-exceptions -fno-rtti, and there is no
# unwinder or pthread implementation on this target. Their presence means the
# link accidentally pulled in hosted libc++ or a wrong archive.
# ---------------------------------------------------------------------------
string(REGEX MATCH
    "[ \t](__cxa_throw|__cxa_begin_catch|__cxa_end_catch|_Unwind_[A-Za-z0-9_]*|__gxx_personality_[A-Za-z0-9_]*|pthread_[A-Za-z0-9_]*)(\r?\n|$)"
    cxa_hit "${nm_all}")
if(cxa_hit)
    message(FATAL_ERROR
        "verify_user_elf: ${ELF} pulls in exception/unwind/thread runtime: ${cxa_hit}")
endif()

# ---------------------------------------------------------------------------
# 4. No SSE / x87 instructions.
#
# The kernel never sets CR4.OSFXSR, so any SSE instruction faults with #UD at
# runtime. Every component is built -mno-sse -msoft-float precisely to prevent
# this. Checking the disassembly catches a flag that silently got dropped.
# ---------------------------------------------------------------------------
execute_process(
    COMMAND ${KYUZEN_OBJDUMP} -d "${ELF}"
    OUTPUT_VARIABLE objdump_out
    ERROR_VARIABLE  objdump_err
    RESULT_VARIABLE objdump_rc
)
if(NOT objdump_rc EQUAL 0)
    message(FATAL_ERROR "verify_user_elf: llvm-objdump failed on ${ELF}: ${objdump_err}")
endif()

string(REGEX MATCH "%(xmm|ymm|zmm)[0-9]+" sse_hit "${objdump_out}")
if(sse_hit)
    message(FATAL_ERROR
        "verify_user_elf: ${ELF} contains SSE instructions (${sse_hit}) — "
        "the kernel does not enable CR4.OSFXSR, so this would fault at runtime")
endif()

string(REGEX MATCH
    "[ \t](fld|fst|fstp|fxch|fucom|fucomi|fadd|fsub|fmul|fdiv|fild|fist|fistp|fcom|fcomp)[ \t]"
    x87_hit "${objdump_out}")
if(x87_hit)
    message(FATAL_ERROR
        "verify_user_elf: ${ELF} contains x87 instructions (${x87_hit}) — "
        "the target is built -msoft-float")
endif()
