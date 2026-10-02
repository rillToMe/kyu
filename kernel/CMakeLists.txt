# ============================================================================
# kernel/CMakeLists.txt — the Ring-0 kernel image (build/bin/myos.bin).
#
# The kernel is assembled from per-subsystem OBJECT libraries so that the
# dependency graph is visible and per-subsystem flags stay local, but the final
# executable links ONE flat object list in the exact order the old Makefile
# used (docs/development/build-system-audit.md §7.2, risk R1).
#
# Link semantics preserved from the old build:
#   ld.lld -flavor gnu -T linker.ld -m elf_x86_64 --build-id=none -nostdlib
#   ENTRY(kernel_main), higher-half at 0xFFFFFFFF80000000
# ============================================================================

# ---------------------------------------------------------------------------
# Source groups. These mirror the audit's subsystem breakdown so that a
# per-subsystem flag (or a future per-subsystem include path) has an obvious
# home, without splitting the single link step.
# ---------------------------------------------------------------------------
set(_arch_sources
    arch/x86/gdt.c
    arch/x86/idt.c
    arch/x86/lapic.c
)

set(_driver_sources
    drivers/acpi.c
    drivers/ata.c
    drivers/keyboard.c
    drivers/mouse.c
    drivers/pci.c
    drivers/rtc.c
    drivers/serial.c
    drivers/timer.c
    drivers/tty.c
)

set(_core_sources
    kernel/boot_console.c
    kernel/cpu.c
    kernel/display.c
    kernel/entropy.c
    kernel/kernel.c
    kernel/kprint.c
    kernel/string.c
    kernel/timer_callbacks.c
)

set(_syscall_sources
    kernel/syscall/sys_fs.c
    kernel/syscall/sys_gpu.c
    kernel/syscall/sys_kwm.c
    kernel/syscall/sys_mem.c
    kernel/syscall/sys_misc.c
    kernel/syscall/sys_net.c
    kernel/syscall/sys_proc.c
    kernel/syscall/sys_system.c
    kernel/syscall/sys_vfs.c
    kernel/syscall/syscall.c
)

set(_sched_sources
    kernel/sched/block.c
    kernel/sched/core.c
    kernel/sched/debug.c
    kernel/sched/lifecycle.c
    kernel/sched/runqueue.c
)

set(_fs_sources
    kernel/fs/bcache.c
    kernel/fs/kfs_balloc.c
    kernel/fs/kfs_dir.c
    kernel/fs/kfs_extent.c
    kernel/fs/kfs_inode.c
    kernel/fs/kfs_shim.c
    kernel/fs/kfs_super.c
    kernel/fs/kfs_vnode.c
    kernel/fs/vfs.c
    kernel/fs/vfs_fd.c
)

set(_mm_sources
    kernel/mm/heap.c
    kernel/mm/heap_watch.c
    kernel/mm/paging.c
    kernel/mm/pmm.c
    kernel/mm/uheap.c
)

set(_sync_sources
    kernel/sync/event.c
    kernel/sync/spinlock.c
    kernel/sync/sync.c
    kernel/sync/wait.c
)

set(_proc_sources
    kernel/proc/elf.c
    kernel/proc/proc.c
    kernel/proc/usercopy.c
)

set(_panic_sources
    kernel/panic/panic.c
    kernel/panic/panic_draw.c
    kernel/panic/panic_explain.c
    kernel/panic/panic_hw.c
)

set(_debug_sources
    kernel/debug/crash_archive.c
    kernel/debug/crashdump.c
    kernel/debug/panic_log.c
)

set(_gfx_sources
    kernel/gfx/compositor.c
    kernel/gfx/fb.c
    kernel/gfx/kwm.c
)

set(_smp_sources
    kernel/smp/smp.c
)

# ---------------------------------------------------------------------------
# Graphics: abstraction + software/virtio backends, the optional Intel Gen12
# backend, and GPU memory.
#
# ORDER IS LOAD-BEARING: the old build's SRC_DIRS listed
#   graphics graphics/backend graphics/backend/intel graphics/memory
# so gpu_alloc.c comes AFTER every Intel file. Object order determines symbol
# layout, and a different order produces a different (still working) binary —
# which would break the byte-for-byte parity check against the Make baseline.
# ---------------------------------------------------------------------------
set(_graphics_sources
    graphics/ghal.c
    graphics/backend/software.c
    graphics/backend/virtio_gpu.c
)

if(KYUZEN_ENABLE_INTEL_GHAL)
    kyuzen_glob(_intel_sources
        "${KYUZEN_ROOT}/graphics/backend/intel/*.c"
    )
    # file(GLOB) returns paths in an unspecified order; Make's $(wildcard) is
    # sorted. Sort explicitly so object order is deterministic and matches the
    # baseline.
    list(SORT _intel_sources)
    list(APPEND _graphics_sources ${_intel_sources})
endif()

# graphics/memory comes after graphics/backend/intel in SRC_DIRS order.
list(APPEND _graphics_sources graphics/memory/gpu_alloc.c)

# ---------------------------------------------------------------------------
# virtio-gpu driver + virtqueue.
# ---------------------------------------------------------------------------
set(_virtio_sources
    drivers/graphics/hw/virtio_gpu_cmd.c
    drivers/graphics/hw/virtio_gpu_dev.c
    drivers/graphics/hw/virtqueue.c
)

# ---------------------------------------------------------------------------
# Colour math — the ONE source compiled for both kernel and user-space.
#
# The kernel copy is defined by libs/gui/color/CMakeLists.txt as
# kyuzen-color-kernel (kernel flags). It is linked below at the same position
# it occupied in the old Makefile's C_SOURCES (last of the C sources).
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Object libraries. INTERFACE linking to kyuzen-flags-kernel gives each one the
# canonical kernel flags without any global state.
#
# The source lists in cmake/KyuzenSources.cmake are written relative to the
# repository root (so they read like the old Makefile's SRC_DIRS). They are
# resolved to absolute paths here, once.
# ---------------------------------------------------------------------------
function(_kyuzen_abs out_var)
    set(_abs "")
    foreach(_f IN LISTS ARGN)
        # file(GLOB ...) already returns absolute paths; only prefix the
        # repo-relative ones from cmake/KyuzenSources.cmake.
        if(IS_ABSOLUTE "${_f}")
            list(APPEND _abs "${_f}")
        else()
            list(APPEND _abs "${KYUZEN_ROOT}/${_f}")
        endif()
    endforeach()
    set(${out_var} "${_abs}" PARENT_SCOPE)
endfunction()

function(_kyuzen_kernel_objlib name)
    _kyuzen_abs(_srcs ${ARGN})
    add_library(${name} OBJECT ${_srcs})
    target_link_libraries(${name} PRIVATE kyuzen-flags-kernel)
    target_include_directories(${name} PRIVATE
        ${KYUZEN_ROOT}/include
        ${KYUZEN_ROOT}/graphics
        ${KYUZEN_ROOT}/graphics/memory
        ${KYUZEN_ROOT}/drivers/graphics/hw
        ${KYUZEN_ROOT}/libs/gui/color/include
    )
endfunction()

_kyuzen_kernel_objlib(kyuzen-kernel-arch     ${_arch_sources})
_kyuzen_kernel_objlib(kyuzen-kernel-drivers  ${_driver_sources})
_kyuzen_kernel_objlib(kyuzen-kernel-core     ${_core_sources})
_kyuzen_kernel_objlib(kyuzen-kernel-syscall  ${_syscall_sources})
_kyuzen_kernel_objlib(kyuzen-kernel-sched    ${_sched_sources})
_kyuzen_kernel_objlib(kyuzen-kernel-fs       ${_fs_sources})
_kyuzen_kernel_objlib(kyuzen-kernel-mm       ${_mm_sources})
_kyuzen_kernel_objlib(kyuzen-kernel-sync     ${_sync_sources})
_kyuzen_kernel_objlib(kyuzen-kernel-proc     ${_proc_sources})
_kyuzen_kernel_objlib(kyuzen-kernel-panic    ${_panic_sources})
_kyuzen_kernel_objlib(kyuzen-kernel-debug    ${_debug_sources})
_kyuzen_kernel_objlib(kyuzen-kernel-gfx      ${_gfx_sources})
_kyuzen_kernel_objlib(kyuzen-kernel-smp      ${_smp_sources})
_kyuzen_kernel_objlib(kyuzen-kernel-graphics ${_graphics_sources})
_kyuzen_kernel_objlib(kyuzen-kernel-virtio   ${_virtio_sources})

# ---------------------------------------------------------------------------
# kernel/net: the four files that include lwIP headers need kyuzen-flags-lwip,
# not the plain kernel flags. This is the same per-file override the old
# Makefile expressed with four explicit pattern rules.
# ---------------------------------------------------------------------------
add_library(kyuzen-kernel-net OBJECT)
_kyuzen_abs(_net_srcs ${KYUZEN_KERNEL_LWIP_CONSUMERS})
target_sources(kyuzen-kernel-net PRIVATE ${_net_srcs})
target_link_libraries(kyuzen-kernel-net PRIVATE kyuzen-flags-lwip)
target_include_directories(kyuzen-kernel-net PRIVATE
    ${KYUZEN_ROOT}/include
    ${KYUZEN_ROOT}/graphics
    ${KYUZEN_ROOT}/libs/gui/color/include
)

# ---------------------------------------------------------------------------
# Assembly objects.
#
# Built with explicit custom commands rather than the ASM_NASM language.
#
# WHY: NASM emits an STT_FILE symbol for every input containing the path
# EXACTLY as given on the command line. The old Makefile ran nasm from the repo
# root with a relative path, producing `arch/x86/exceptions.asm`. CMake's
# ASM_NASM support always passes an absolute path, which would produce
# `E:/.../kyuzen-os/arch/x86/exceptions.asm` — 55 bytes longer per file. That
# changes .strtab, which shifts every subsequent byte and makes the kernel
# differ from the Make baseline even though the code is identical.
#
# Driving nasm directly with a relative source path and WORKING_DIRECTORY set
# to the repo root reproduces the original invocation byte for byte.
#
# The include directory is the repo root because the .asm files say
# `%include "arch/x86/isr_macro.inc"` — relative to the root, matching how the
# old Makefile always invoked nasm from there.
# ---------------------------------------------------------------------------
set(_asm_objects "")

foreach(_asm IN LISTS KYUZEN_KERNEL_ASM_SOURCES)
    string(REPLACE "/" "_" _asm_obj_name "${_asm}")
    set(_asm_obj "${CMAKE_BINARY_DIR}/obj/asm/${_asm_obj_name}.o")
    list(APPEND _asm_objects "${_asm_obj}")

    add_custom_command(
        OUTPUT "${_asm_obj}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/obj/asm"
        COMMAND "${KYUZEN_NASM}"
                -f elf64
                -MD "${_asm_obj}.d"
                -I${KYUZEN_ROOT}          # joined form: no shell quoting issues
                "${_asm}"                 # RELATIVE — see note above
                -o "${_asm_obj}"
        DEPENDS
            "${KYUZEN_ROOT}/${_asm}"
            "${KYUZEN_ROOT}/arch/x86/isr_macro.inc"
        WORKING_DIRECTORY "${KYUZEN_ROOT}"
        COMMENT "Assembling ${_asm} (nasm)"
        VERBATIM
    )
endforeach()

set_source_files_properties(${_asm_objects} PROPERTIES
    EXTERNAL_OBJECT TRUE
    GENERATED TRUE
)

# ---------------------------------------------------------------------------
# The kernel executable.
#
# LINK ORDER reproduces the old Makefile's ALL_OBJS exactly:
#
#     ALL_OBJS = OBJS + LWIP_OBJS + E1000_OBJS
#     OBJS     = $(C_SOURCES) + $(ASM_SOURCES)
#
# i.e. C sources (in SRC_DIRS order) -> ASM sources -> lwIP -> e1000.
#
# Order determines symbol layout and therefore the exact bytes of the linked
# binary. Reproducing it is what makes byte-for-byte parity with the Make
# baseline achievable; a different order yields a working but different image.
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# The kernel executable.
#
# Uses the same placeholder-object technique as the app helper: an empty
# translation unit as the nominal source, with all real content supplied as
# $<TARGET_OBJECTS:...> so the link order is exactly ALL_OBJS order.
# ---------------------------------------------------------------------------
kyuzen_placeholder_object(_kernel_placeholder)

add_executable(kyuzen-kernel "${_kernel_placeholder}")
set_source_files_properties("${_kernel_placeholder}" PROPERTIES
    EXTERNAL_OBJECT TRUE
    GENERATED TRUE
)
set_target_properties(kyuzen-kernel PROPERTIES LINKER_LANGUAGE C)

# Make sure the assembly objects exist before the link.
add_custom_target(kyuzen-kernel-asm-objects DEPENDS ${_asm_objects})
add_dependencies(kyuzen-kernel kyuzen-kernel-asm-objects)

# ---------------------------------------------------------------------------
# Object ORDER.
#
# `$<TARGET_OBJECTS:...>` is the one form CMake places at the exact position it
# appears among the link items. Passing an object library directly, an
# INTERFACE library's sources, or raw .o paths all get re-ordered (CMake hoists
# objects to the front or appends them at the end), which changes symbol
# layout and therefore the bytes of the linked kernel.
#
# The assembly objects are interleaved as plain paths via the
# KYUZEN_ASM_INTERLEAVED variable set below.
# ---------------------------------------------------------------------------
target_link_libraries(kyuzen-kernel PRIVATE
    # --- C sources, in SRC_DIRS order ---------------------------------------
    $<TARGET_OBJECTS:kyuzen-kernel-arch>
    $<TARGET_OBJECTS:kyuzen-kernel-drivers>
    $<TARGET_OBJECTS:kyuzen-kernel-core>
    $<TARGET_OBJECTS:kyuzen-kernel-syscall>
    $<TARGET_OBJECTS:kyuzen-kernel-smp>
    $<TARGET_OBJECTS:kyuzen-kernel-gfx>
    $<TARGET_OBJECTS:kyuzen-kernel-sched>
    $<TARGET_OBJECTS:kyuzen-kernel-fs>
    $<TARGET_OBJECTS:kyuzen-kernel-net>
    $<TARGET_OBJECTS:kyuzen-kernel-mm>
    $<TARGET_OBJECTS:kyuzen-kernel-debug>
    $<TARGET_OBJECTS:kyuzen-kernel-sync>
    $<TARGET_OBJECTS:kyuzen-kernel-proc>
    $<TARGET_OBJECTS:kyuzen-kernel-panic>
    $<TARGET_OBJECTS:kyuzen-kernel-graphics>
    $<TARGET_OBJECTS:kyuzen-kernel-virtio>
    $<TARGET_OBJECTS:kyuzen-color-kernel>
    # --- assembly (before lwIP, as in ALL_OBJS) -----------------------------
    ${_asm_objects}
    # --- lwIP + NIC ---------------------------------------------------------
    $<TARGET_OBJECTS:kyuzen-lwip>
    $<TARGET_OBJECTS:kyuzen-e1000>
)

# ---------------------------------------------------------------------------
# Link with ld.lld DIRECTLY.
#
# The old build ran ld.lld itself: `$(LD) $(LDFLAGS) $(ALL_OBJS) -o $@`, with
# LLD's own flags. Those are LLD options, not clang-driver options — and on
# this host the clang driver would delegate to GCC's linker anyway, which does
# not understand elf_x86_64.
#
# `-flavor gnu` MUST BE FIRST. ld.lld accepts it only as its very first
# argument; once any other option is seen it has already committed to a
# flavour and rejects -flavor. The old Makefile satisfied this by putting
# -flavor gnu at the head of LDFLAGS.
# ---------------------------------------------------------------------------
target_link_options(kyuzen-kernel PRIVATE
    -flavor gnu
    -T ${KYUZEN_ROOT}/linker.ld
    -m elf_x86_64
    --build-id=none
    -nostdlib
)

set_target_properties(kyuzen-kernel PROPERTIES
    OUTPUT_NAME "myos"
    SUFFIX ".bin"
    RUNTIME_OUTPUT_DIRECTORY "${KYUZEN_BIN_DIR}"
    LINK_DEPENDS "${KYUZEN_ROOT}/linker.ld"
)

# ---------------------------------------------------------------------------
# Compatibility copy: build/myos.bin.
#
# The old Makefile kept this so local scripts pointing at the old path keep
# working. It is written only when the content actually changes (cmp -s), so a
# no-op build does not churn the timestamp and re-trigger the ISO.
# ---------------------------------------------------------------------------
add_custom_command(TARGET kyuzen-kernel POST_BUILD
    COMMAND ${CMAKE_COMMAND}
            -DSRC=$<TARGET_FILE:kyuzen-kernel>
            -DDST=${CMAKE_BINARY_DIR}/myos.bin
            -P ${KYUZEN_ROOT}/cmake/copy_if_different.cmake
    COMMENT "Updating compatibility copy build/myos.bin"
    VERBATIM
)
