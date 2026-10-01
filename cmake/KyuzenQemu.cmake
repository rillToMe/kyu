# ============================================================================
# KyuzenQemu.cmake — QEMU run targets.
#
# Only the workflows that already existed (BUILD_SYSTEM_AUDIT.md §11). The
# hardware configuration is copied verbatim from the old Makefile: changing it
# would silently change what is being tested.
#
#   cmake --build build --target run
#   cmake --build build --target run-serial
#   cmake --build build --target run-wd
# ============================================================================

include_guard(GLOBAL)

# ---------------------------------------------------------------------------
# Host display settings, overridable the same way the Makefile allowed.
#   -DKYUZEN_QEMU_DISPLAY=none          (headless)
#   -DKYUZEN_QEMU_FULLSCREEN=OFF        (windowed)
# ---------------------------------------------------------------------------
set(KYUZEN_QEMU_DISPLAY "gtk,zoom-to-fit=on" CACHE STRING
    "QEMU -display value")
option(KYUZEN_QEMU_FULLSCREEN "Start QEMU full-screen" ON)

set(_qemu_fullscreen_arg "")
if(KYUZEN_QEMU_FULLSCREEN)
    set(_qemu_fullscreen_arg "-full-screen")
endif()

# Disk image for the boot drive. The old Makefile used disk.img in the repo
# root; it is gitignored and created by the OS itself on first boot.
set(KYUZEN_DISK_IMAGE "${KYUZEN_ROOT}/disk.img" CACHE FILEPATH
    "Raw disk image attached as the boot drive")
set(KYUZEN_SERIAL_LOG "${KYUZEN_ROOT}/serial.log" CACHE FILEPATH
    "COM1 mirror file")

# ---------------------------------------------------------------------------
# Common QEMU argument list.
# ---------------------------------------------------------------------------
function(_kyuzen_qemu_args out_var)
    cmake_parse_arguments(ARG "" "SMP" "EXTRA" ${ARGN})

    set(_args
        -cpu max
        -m 1G
        -boot d
        -smp ${ARG_SMP}
        -drive "file=${KYUZEN_DISK_IMAGE},format=raw,index=0,media=disk"
        -drive "file=${KYUZEN_ISO_IMAGE},media=cdrom,index=2"
        -nic user,model=e1000
        -vga none
        -device virtio-vga,xres=1920,yres=1080
        -display ${KYUZEN_QEMU_DISPLAY}
        ${_qemu_fullscreen_arg}
    )
    if(ARG_EXTRA)
        list(APPEND _args ${ARG_EXTRA})
    endif()

    set(${out_var} ${_args} PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# run — the default workflow. 8 CPUs, serial mirrored to serial.log.
#
# The old recipe deleted serial.log first so the tail of the file is always
# the current run's trace.
# ---------------------------------------------------------------------------
_kyuzen_qemu_args(_run_args SMP 8 EXTRA -serial "file:${KYUZEN_SERIAL_LOG}")

add_custom_target(run
    COMMAND ${CMAKE_COMMAND} -E rm -f "${KYUZEN_SERIAL_LOG}"
    COMMAND ${KYUZEN_QEMU} ${_run_args}
    DEPENDS kyuzen-image
    COMMENT "Booting KyuzenOS in QEMU (serial -> ${KYUZEN_SERIAL_LOG})"
    USES_TERMINAL
    VERBATIM
)

# ---------------------------------------------------------------------------
# run-serial — serial on stdio, so a panic dump lands in the terminal.
# ---------------------------------------------------------------------------
_kyuzen_qemu_args(_run_serial_args SMP 4 EXTRA -serial stdio)

add_custom_target(run-serial
    COMMAND ${KYUZEN_QEMU} ${_run_serial_args}
    DEPENDS kyuzen-image
    COMMENT "Booting KyuzenOS in QEMU (serial -> stdio)"
    USES_TERMINAL
    VERBATIM
)

# ---------------------------------------------------------------------------
# run-wd — serial to a file. More reliable than stdio on Windows.
# ---------------------------------------------------------------------------
_kyuzen_qemu_args(_run_wd_args SMP 4 EXTRA -serial "file:${KYUZEN_SERIAL_LOG}")

add_custom_target(run-wd
    COMMAND ${CMAKE_COMMAND} -E rm -f "${KYUZEN_SERIAL_LOG}"
    COMMAND ${KYUZEN_QEMU} ${_run_wd_args}
    DEPENDS kyuzen-image
    COMMENT "Booting KyuzenOS in QEMU (serial -> ${KYUZEN_SERIAL_LOG})"
    USES_TERMINAL
    VERBATIM
)
