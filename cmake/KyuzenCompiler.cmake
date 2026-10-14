# ============================================================================
# KyuzenCompiler.cmake — canonical compiler flag sets.
#
# Every flag set below mirrors one variable from the existing Makefile. They
# are exposed as INTERFACE libraries so consumers write:
#
#     target_link_libraries(my-target PRIVATE kyuzen-flags-kernel)
#
# and never touch global state. This is the single place where a flag set is
# defined; no target re-declares flags inline.
#
# Mapping to the old Makefile (docs/development/build-system-audit.md §4, Appendix B):
#
#   kyuzen-flags-kernel      <- CFLAGS
#   kyuzen-flags-lwip        <- LWIP_CFLAGS
#   kyuzen-flags-app         <- CFLAGS_APP
#   kyuzen-flags-applib      <- CFLAGS_LIB
#   kyuzen-flags-widget      <- CXXFLAGS_LIB
#   kyuzen-flags-sdk-c       <- SDK_CFLAGS
#   kyuzen-flags-sdk-cpp     <- SDK_CXXFLAGS
#   kyuzen-flags-libc-port   <- LIBC_PORT_CFLAGS
#   kyuzen-flags-freetype    <- FT_KYUZEN_CFLAGS
#   kyuzen-flags-lexbor      <- LEXBOR_KYUZEN_CFLAGS
#   kyuzen-flags-bearssl-os  <- BL_OS_FLAGS
#
# NOTE on dependency tracking: the Makefile passes `-MMD -MP` explicitly.
# CMake + Ninja track header dependencies natively (it generates its own
# depfile arguments), so those two flags are intentionally NOT repeated here.
# Adding them would be harmless but redundant.
#
# NOTE on FPU/SSE: the KERNEL is built without SSE (KYUZEN_KERNEL_FP_BAN) and
# userspace is NOT. This asymmetry is deliberate and load-bearing — interrupt
# entry does not save XMM, so kernel FP/SSE code would corrupt a running
# ring-3 task's registers. See the KYUZEN_KERNEL_FP_BAN comment below for the
# full reasoning, and docs/kernel/arch-hardening.md for the FPU model.
#
# Userspace components therefore no longer carry -mno-sse/-msoft-float. Their
# FPU state IS preserved across tasks, by fpu_switch() (kernel/sched/fpu.c),
# which is why they can use float/double and libc functions that return them.
# ============================================================================

include_guard(GLOBAL)

# ---------------------------------------------------------------------------
# Common freestanding base shared by every target-build flag set.
#
# NO SSE/FP FLAGS HERE. Whether FPU instructions may be emitted is a per-domain
# decision (kernel vs userspace), and the two domains need OPPOSITE answers —
# see KYUZEN_KERNEL_FP_BAN below. Putting -mno-sse in this shared base would
# silently re-ban it for userspace, which is exactly what this change removes.
# ---------------------------------------------------------------------------
set(KYUZEN_FREESTANDING_BASE
    -ffreestanding
    -nostdlib
    -mno-red-zone
)

# ---------------------------------------------------------------------------
# KYUZEN_KERNEL_FP_BAN — the kernel must never emit FP/SSE instructions.
#
# WHY THIS IS NOT OPTIONAL, AND WHY IT IS NOT JUST CONSERVATISM
#
#   FPU state is saved and restored per task by fpu_switch() (kernel/sched/
#   fpu.c), and that runs ONLY on a context switch. Interrupt entry does NOT
#   save XMM: arch/x86/isr_macro.inc pushes the general-purpose registers only
#   (PUSHA64), and nothing else in arch/ or kernel/ touches fxsave.
#
#   So if kernel code emitted SSE, an interrupt taken while a ring-3 task was
#   using XMM would run kernel code that clobbers those registers, and the task
#   would resume with corrupted FP state. A user-space computation would
#   silently produce wrong results depending on where the timer fired — the
#   worst class of bug this project can have.
#
#   This is not hypothetical: kernel/string.c compiled without -mno-sse emits
#   `movups %xmm0` in memcpy, and memcpy is reachable from scheduler and
#   filesystem paths that run inside interrupts.
#
#   Userspace is different: it IS covered, because fpu_switch() runs between
#   tasks. That asymmetry is the whole design, and it is why userspace is
#   opened up (see the flag sets below) while the kernel stays closed.
#
#   Keeping the ban here also means the kernel stays deterministic and never
#   depends on FPU state a context switch might have replaced.
#
# The check is enforced mechanically after the link (cmake/verify_kernel_elf
# .cmake), so a dropped flag is a build error rather than a boot-time mystery.
# ---------------------------------------------------------------------------
set(KYUZEN_KERNEL_FP_BAN
    -mno-sse
    -mno-sse2
    -mno-mmx
    -msoft-float
)

# ===========================================================================
# KERNEL  (Makefile: CFLAGS)
#
# NOTE: -Wall is deliberately NOT added. The old kernel CFLAGS did not include
# it, and the verified baseline builds with zero warnings. Adding -Wall here
# would surface pre-existing warnings in third-party-derived code (e.g.
# drivers/net/e1000) that the project has not chosen to fix, making the
# migration look like it introduced them. Enabling stricter warnings is a
# separate, deliberate change — not part of a build-system migration.
# ===========================================================================
add_library(kyuzen-flags-kernel INTERFACE)
target_compile_options(kyuzen-flags-kernel INTERFACE
    ${KYUZEN_FREESTANDING_BASE}
    ${KYUZEN_KERNEL_FP_BAN}
    -O2
    -mcmodel=kernel          # required for the higher-half kernel: prevents
                             # R_X86_64_32 relocations for symbols above 4 GiB
    -std=c11
)
target_compile_definitions(kyuzen-flags-kernel INTERFACE
    ATA_READ_PATH_DEFAULT=${KYUZEN_ATA_READ_PATH_DEFAULT}
)

# ===========================================================================
# lwIP  (Makefile: LWIP_CFLAGS = CFLAGS + lwIP includes + -Wno-error)
#
# Applied to the 32 lwIP sources plus the 4 kernel/net/*.c files that include
# lwIP headers (net_init, net_ping, net_socket, net_dns).
# ===========================================================================
add_library(kyuzen-flags-lwip INTERFACE)
target_link_libraries(kyuzen-flags-lwip INTERFACE kyuzen-flags-kernel)
target_compile_options(kyuzen-flags-lwip INTERFACE
    -std=c11
    -Wno-error               # internal lwIP warnings must not fail the build
)
target_include_directories(kyuzen-flags-lwip INTERFACE
    ${KYUZEN_ROOT}/third_party/net/lwip/src/include
    ${KYUZEN_ROOT}/drivers/net/port
)

# ===========================================================================
# USER-SPACE C — applications  (Makefile: CFLAGS_APP = CFLAGS_COMMON -O0)
# ===========================================================================
add_library(kyuzen-flags-app INTERFACE)
target_compile_options(kyuzen-flags-app INTERFACE
    ${KYUZEN_FREESTANDING_BASE}
    -O0                      # apps build unoptimized, as before
)
target_include_directories(kyuzen-flags-app INTERFACE
    ${KYUZEN_ROOT}/include
    ${KYUZEN_ROOT}/libs/gui/color/include
)

# ===========================================================================
# USER-SPACE C — libraries  (Makefile: CFLAGS_LIB = CFLAGS_COMMON -O2)
# ===========================================================================
add_library(kyuzen-flags-applib INTERFACE)
target_compile_options(kyuzen-flags-applib INTERFACE
    ${KYUZEN_FREESTANDING_BASE}
    -O2
)
target_include_directories(kyuzen-flags-applib INTERFACE
    ${KYUZEN_ROOT}/include
    ${KYUZEN_ROOT}/libs/gui/color/include
)

# ===========================================================================
# USER-SPACE C++ — widget toolkit  (Makefile: CXXFLAGS_LIB)
#
# No exceptions, no RTTI, no __cxa_atexit, no thread-safe statics: there is no
# C++ runtime on this target. operator new/delete and __cxa_pure_virtual are
# stubbed once in libs/gui/widget/src/runtime/runtime.cpp (ODR).
# apps/app.ld uses ENTRY(main) and does NOT run .init_array, so global
# constructors must not be used by anything built with this flag set.
# ===========================================================================
add_library(kyuzen-flags-widget INTERFACE)
target_compile_options(kyuzen-flags-widget INTERFACE
    ${KYUZEN_FREESTANDING_BASE}
    -O2
    -fno-exceptions
    -fno-rtti
    -fno-use-cxa-atexit
    -fno-threadsafe-statics
    -std=c++17
)
target_include_directories(kyuzen-flags-widget INTERFACE
    ${KYUZEN_ROOT}/include
    ${KYUZEN_ROOT}/libs/gui/color/include
)

# ===========================================================================
# C SDK applications  (Makefile: SDK_CFLAGS = LIBC_TARGET_FLAGS -O2)
#
# LIBC_TARGET_FLAGS is the canonical freestanding set shared with the LLVM
# libc build itself, so an app and the libc it links agree on the ABI.
# ===========================================================================
add_library(kyuzen-flags-sdk-c INTERFACE)
target_compile_options(kyuzen-flags-sdk-c INTERFACE
    ${KYUZEN_FREESTANDING_BASE}
    -O2
)

# ===========================================================================
# C++ SDK applications  (Makefile: SDK_CXXFLAGS)
#
# -nostdinc++ keeps the compile hermetic from the host libc++; <stddef.h> and
# friends still come from Clang's freestanding headers.
# ===========================================================================
add_library(kyuzen-flags-sdk-cpp INTERFACE)
target_compile_options(kyuzen-flags-sdk-cpp INTERFACE
    ${KYUZEN_FREESTANDING_BASE}
    -O2
    -std=c++17
    -fno-exceptions
    -fno-rtti
    -nostdinc++
)

# ===========================================================================
# LLVM libc port layer  (Makefile: LIBC_PORT_CFLAGS)
#
# Compiles against libc's INTERNAL headers with its internal namespace, so it
# can replace the built-in freelist_heap. LIBC_NAMESPACE must match the pinned
# libc version string exactly.
# ===========================================================================
add_library(kyuzen-flags-libc-port INTERFACE)
target_compile_options(kyuzen-flags-libc-port INTERFACE
    ${KYUZEN_FREESTANDING_BASE}
    -std=gnu++17
    -fno-exceptions
    -fno-rtti
    -O2
    -fno-builtin
    -fno-unwind-tables
    -fno-asynchronous-unwind-tables
    -fvisibility-inlines-hidden
)
target_compile_definitions(kyuzen-flags-libc-port INTERFACE
    LIBC_NAMESPACE=${KYUZEN_LIBC_NAMESPACE}
    LIBC_FULL_BUILD
    LIBC_TARGET_OS_IS_BAREMETAL
    LIBC_ERRNO_MODE=LIBC_ERRNO_MODE_EXTERNAL
    LIBC_THREAD_MODE=LIBC_THREAD_MODE_SINGLE
    LIBC_COPT_PUBLIC_PACKAGING
)

# ===========================================================================
# FreeType freestanding  (Makefile: FT_KYUZEN_CFLAGS)
#
# The Makefile wraps the two FT_CONFIG_*_H macros in single quotes because a
# shell would otherwise eat the angle brackets. CMake passes arguments
# directly with no shell, so the quotes are dropped here (see
# docs/development/build-system-audit.md §4.7 / risk R2).
# ===========================================================================
add_library(kyuzen-flags-freetype INTERFACE)
target_compile_options(kyuzen-flags-freetype INTERFACE
    ${KYUZEN_FREESTANDING_BASE}
    -O2
    -std=c11
)
target_compile_definitions(kyuzen-flags-freetype INTERFACE
    FT2_BUILD_LIBRARY
    FT_CONFIG_STANDARD_LIBRARY_H=<ftkz_stdlib.h>
    FT_CONFIG_MODULES_H=<ftkz_modules.h>
    KZFONT_USE_FREETYPE
)
target_include_directories(kyuzen-flags-freetype INTERFACE
    ${KYUZEN_ROOT}/third_party/freetype/kyuzen/include
    ${KYUZEN_ROOT}/third_party/freetype/include
    ${KYUZEN_ROOT}/libs/text/include
    ${KYUZEN_ROOT}/libs/gui/color/include
)

# ===========================================================================
# Lexbor freestanding  (Makefile: LEXBOR_KYUZEN_CFLAGS)
#
# Include ORDER is load-bearing: the Kyuzen memory.h shim in kyuzen/include
# must win over any host <memory.h>.
# ===========================================================================
add_library(kyuzen-flags-lexbor INTERFACE)
target_compile_options(kyuzen-flags-lexbor INTERFACE
    ${KYUZEN_FREESTANDING_BASE}
    -O2
    -std=c11
)
target_compile_definitions(kyuzen-flags-lexbor INTERFACE
    LEXBOR_STATIC
)
target_include_directories(kyuzen-flags-lexbor INTERFACE
    ${KYUZEN_ROOT}/third_party/lexbor/kyuzen/include
    ${KYUZEN_ROOT}/third_party/lexbor/source
)

# ===========================================================================
# BearSSL — OS build (browser.elf only)  (Makefile: BL_OS_FLAGS)
#
# The -DBR_* switches disable every built-in entropy/time source: the OS port
# supplies randomness and time through Kyuzen syscalls instead
# (apps/browser/tls/tls_kyuzen.c).
#
# BR_AES_X86NI / BR_SSE2 stay off: the x86 AES-NI path is written against
# compiler intrinsics that assume a hosted <wmmintrin.h> setup this target does
# not provide, and the portable C path is already correct. This is no longer
# about SSE faulting — userspace may use SSE since Phase 3 (fpu_switch()
# preserves XMM). It is about not depending on intrinsics headers that are not
# part of the freestanding SDK.
# ===========================================================================
add_library(kyuzen-flags-bearssl-os INTERFACE)
target_compile_options(kyuzen-flags-bearssl-os INTERFACE
    ${KYUZEN_FREESTANDING_BASE}
    -O2
    -std=c11
)
target_compile_definitions(kyuzen-flags-bearssl-os INTERFACE
    BR_USE_UNIX_TIME=0
    BR_USE_WIN32_TIME=0
    BR_USE_URANDOM=0
    BR_USE_WIN32_RAND=0
    BR_RDRAND=0
    BR_AES_X86NI=0
    BR_SSE2=0
)
target_include_directories(kyuzen-flags-bearssl-os INTERFACE
    ${KYUZEN_ROOT}/third_party/bearssl/inc
    ${KYUZEN_ROOT}/third_party/bearssl/src
    ${KYUZEN_ROOT}/include
    ${KYUZEN_ROOT}/apps/browser/tls
)
