# ============================================================================
# KyuzenImage.cmake — bootable ISO assembly.
#
# Reproduces the old Makefile's $(ISO_IMAGE) recipe step for step
# (docs/development/build-system-audit.md §9). Two things matter more than the rest:
#
#   * The MODULE GUARD. limine.conf lists 86 modules; Limine panics at BOOT
#     with "Failed to open module with path" if any is missing, and that is
#     invisible at build time. The guard turns it into a build error listing
#     exactly what is missing. It exists because neofect.json was once listed
#     but never staged — a bug that shipped.
#
#   * DEPENDS on real targets. The old build needed an order-only prerequisite
#     on a phony `apps` target to force the ELFs to exist, with four ELFs
#     excluded to avoid a recursive-make fork-bomb (audit §13.3). Depending on
#     the actual targets makes that whole class of bug impossible.
# ============================================================================

include_guard(GLOBAL)

function(kyuzen_add_image_target)
    # -----------------------------------------------------------------------
    # Static inputs.
    # -----------------------------------------------------------------------
    set(_limine_files
        "${KYUZEN_ROOT}/limine/BOOTX64.EFI"
        "${KYUZEN_ROOT}/limine/limine-bios.sys"
        "${KYUZEN_ROOT}/limine/limine-bios-cd.bin"
        "${KYUZEN_ROOT}/limine/limine-uefi-cd.bin"
    )

    # Fail early and clearly if a required Limine artifact is absent. These are
    # gitignored (limine.exe by limine/.gitignore, *.bin by the root
    # .gitignore) yet mandatory — a fresh clone cannot build an ISO
    # (docs/development/build-system-audit.md §13.5).
    foreach(_f IN LISTS _limine_files)
        if(NOT EXISTS "${_f}")
            message(FATAL_ERROR
                "ISO: required Limine artifact is missing: ${_f}\n"
                "It is gitignored but mandatory. Restore it (or fetch the "
                "Limine release) before building the image.")
        endif()
    endforeach()

    if(NOT EXISTS "${KYUZEN_ROOT}/limine/limine.exe")
        message(FATAL_ERROR
            "ISO: limine/limine.exe is missing (needed for `bios-install`).\n"
            "It is gitignored and is NOT built by this project — build it once "
            "with `make -C limine` (or `cc -O2 limine.c -o limine.exe`).")
    endif()

    file(GLOB _manifests "${KYUZEN_ROOT}/manifests/*.app")

    set(_desktop_assets
        assets/icons/default.png
        assets/icons/demo.png
        assets/icons/clocks.png
        assets/icons/folder.png
        assets/icons/notepad.png
        assets/icons/settings.png
        assets/icons/terminal.png
        assets/icons/image_view.png
        assets/icons/taskmanager.png
        assets/icons/calculator.png
        assets/icons/gallery.png
        assets/icons/font-app.png
        assets/wallpaper/island.png
        assets/wallpaper/black-hole.png
        assets/wallpaper/city-lanscaps.png
        assets/wallpaper/city-town.png
        assets/wallpaper/kimi-no-nawa.png
        assets/wallpaper/meadow.png
    )

    # Font list is kept in sync with KZ_FONT_FILES in libs/text/include/kzfonts.h.
    set(_font_assets
        assets/fonts/Inter-Regular.ttf
        assets/fonts/DejaVuSans.ttf
        assets/fonts/NotoSansMono-Regular.ttf
        assets/fonts/NotoSansMono-Bold.ttf
        assets/fonts/NotoSansAdlam-Regular.ttf
    )

    # neofetch art, read at runtime by cmd_neofetch (system/shell_core.c).
    set(_shell_assets
        assets/shell/neofect.json
    )

    set(_logo_assets
        assets/logo/kyuzen.png
        assets/logo/logo-splash.png
    )

    set(_flat_assets ${_desktop_assets} ${_font_assets} ${_shell_assets})
    list(TRANSFORM _flat_assets PREPEND "${KYUZEN_ROOT}/")
    list(TRANSFORM _logo_assets  PREPEND "${KYUZEN_ROOT}/")
    list(TRANSFORM _manifests    PREPEND "")

    # -----------------------------------------------------------------------
    # App ELFs. Collected from the targets that actually build them, so the
    # ISO cannot run ahead of the build.
    # -----------------------------------------------------------------------
    set(_app_targets "")
    foreach(_t IN LISTS KYUZEN_APP_TARGETS)
        if(TARGET ${_t})
            list(APPEND _app_targets ${_t})
        endif()
    endforeach()

    set(_app_elf_files "")
    foreach(_t IN LISTS _app_targets)
        list(APPEND _app_elf_files "$<TARGET_FILE:${_t}>")
    endforeach()

    # Rust ELFs are produced by a custom target rather than per-app targets, so
    # they are listed as paths and the target is added to DEPENDS below.
    set(_rust_deps "")
    if(TARGET ${KYUZEN_RUST_TARGET})
        list(APPEND _rust_deps ${KYUZEN_RUST_TARGET})
        list(APPEND _app_elf_files ${KYUZEN_RUST_ELFS})
    endif()

    # -----------------------------------------------------------------------
    # Optional extra ELFs (smoke apps) — copied only when their target exists.
    # The old Makefile guarded each one with `if [ -f ... ]`.
    # -----------------------------------------------------------------------
    set(_optional_elfs "")
    foreach(_t IN LISTS KYUZEN_OPTIONAL_ELF_TARGETS)
        if(TARGET ${_t})
            list(APPEND _optional_elfs "$<TARGET_FILE:${_t}>")
        endif()
    endforeach()

    add_custom_command(
        OUTPUT "${KYUZEN_ISO_IMAGE}"
        # --- staging ---------------------------------------------------------
        COMMAND ${CMAKE_COMMAND} -E make_directory "${KYUZEN_ISO_ROOT}/EFI/BOOT"
        # Sweep stale ELFs so an app that was removed does not linger in the
        # image (the old recipe did `rm -f $(ISO_ROOT)/*.elf`).
        COMMAND ${CMAKE_COMMAND}
                -DISO_ROOT=${KYUZEN_ISO_ROOT}
                -DMODE=sweep
                -P ${KYUZEN_ROOT}/cmake/iso_stage.cmake
        # ELFs + kernel + config + manifests + Limine files.
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${_app_elf_files}
                "$<TARGET_FILE:kyuzen-kernel>"
                "${KYUZEN_ROOT}/limine.conf"
                "${KYUZEN_ROOT}/assets/logo/kyuzen.png"
                ${_manifests}
                ${_limine_files}
                "${KYUZEN_ISO_ROOT}/"
        # logo-splash.png is renamed to logo.png in the image.
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${KYUZEN_ROOT}/assets/logo/logo-splash.png"
                "${KYUZEN_ISO_ROOT}/logo.png"
        # Flat assets (icons, wallpapers, fonts, shell art).
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${_flat_assets}
                "${KYUZEN_ISO_ROOT}/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${KYUZEN_ROOT}/limine/BOOTX64.EFI"
                "${KYUZEN_ISO_ROOT}/EFI/BOOT/"
        # Optional smoke-test ELFs, only when those targets exist.
        COMMAND ${CMAKE_COMMAND}
                -DISO_ROOT=${KYUZEN_ISO_ROOT}
                -DMODE=optional
                "-DOPTIONAL_FILES=${_optional_elfs}"
                -P ${KYUZEN_ROOT}/cmake/iso_stage.cmake
        # --- guard: every module_path in limine.conf must be present --------
        COMMAND ${CMAKE_COMMAND}
                -DISO_ROOT=${KYUZEN_ISO_ROOT}
                -DLIMINE_CONF=${KYUZEN_ROOT}/limine.conf
                -P ${KYUZEN_ROOT}/cmake/iso_module_guard.cmake
        # --- build the hybrid BIOS+UEFI image --------------------------------
        # xorriso is run from the staging directory's parent with a RELATIVE
        # directory argument. An absolute Windows path gets mangled by MSYS
        # path translation ("E:/..." is prepended to the current directory),
        # which makes xorriso look for a nonsensical path.
        COMMAND xorriso -as mkisofs
                -b limine-bios-cd.bin
                -no-emul-boot -boot-load-size 4 -boot-info-table
                --efi-boot limine-uefi-cd.bin
                -efi-boot-part --efi-boot-image
                --protective-msdos-label
                iso_root -o boot_image.iso
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        # --- install the BIOS boot stages ------------------------------------
        COMMAND "${KYUZEN_ROOT}/limine/limine.exe" bios-install "${KYUZEN_ISO_IMAGE}"
        DEPENDS
            kyuzen-kernel
            ${_app_targets}
            ${_rust_deps}
            "${KYUZEN_ROOT}/limine.conf"
            ${_manifests}
            ${_limine_files}
            ${_flat_assets}
            ${_logo_assets}
        COMMENT "Assembling KyuzenOS bootable ISO"
        VERBATIM
        USES_TERMINAL
    )
    add_custom_target(kyuzen-image DEPENDS "${KYUZEN_ISO_IMAGE}")

    # -----------------------------------------------------------------------
    # Convenience alias for the old `make boot_image.iso` habit. Named
    # iso (not boot_image.iso) because a custom target must not share a name
    # with the file it produces — Ninja rejects that as a duplicate rule.
    # -----------------------------------------------------------------------
    add_custom_target(iso DEPENDS "${KYUZEN_ISO_IMAGE}")
endfunction()
