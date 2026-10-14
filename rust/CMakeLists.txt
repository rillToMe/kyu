# ============================================================================
# rust/CMakeLists.txt — Rust applications (kept as a separate build domain).
#
# Rule 17: Rust stays a Cargo workspace. CMake does not try to compile Rust
# sources; it invokes `cargo build --release` and copies the resulting ELFs into
# build/apps/, which is what the old Makefile's `rust-apps` target did.
#
# The two things that matter for correctness:
#
#   1. RUSTFLAGS must inject the linker script. The path is relative and Cargo
#      cannot expand relative paths inside rustflags, so it is passed on the
#      command line (the old Makefile did the same, with a comment explaining
#      why it is not in rust/.cargo/config.toml).
#
#   2. The ELFs are copied ONLY when their content changes. An unconditional
#      copy updates the mtime, which makes the ISO look stale and rebuild on
#      every invocation.
# ============================================================================

find_program(KYUZEN_CARGO NAMES cargo)
if(NOT KYUZEN_CARGO)
    message(WARNING
        "rust: cargo not found — the Rust applications will not be built.\n"
        "Install Rust (rustup) or configure with -DKYUZEN_BUILD_RUST=OFF to "
        "silence this warning.")
    return()
endif()

set(_rust_dir "${KYUZEN_ROOT}/rust")
set(_rust_target "x86_64-unknown-none")
set(_rust_out "${_rust_dir}/target/${_rust_target}/release")
set(_rust_names hello-slint control-center)

# The workspace must exist; a missing Cargo.toml means the Rust apps were
# removed, which is a configuration error rather than something to ignore.
if(NOT EXISTS "${_rust_dir}/Cargo.toml")
    message(FATAL_ERROR
        "rust: ${_rust_dir}/Cargo.toml not found, but KYUZEN_BUILD_RUST is ON")
endif()

# ---------------------------------------------------------------------------
# Cargo invocation.
#
# RUSTFLAGS injects the linker script. The path is RELATIVE and Cargo cannot
# expand relative paths inside rustflags (the old Makefile documents this), so
# it is passed on the command line rather than in rust/.cargo/config.toml.
#
# `-C relocation-model=static` matches the workspace's freestanding setup and
# `-C link-arg=-T../apps/app.ld` points the linker at the shared user-app script
# (single PT_LOAD at 0x4000000) — without it rust-lld falls back to its default
# layout and produces a host-shaped binary.
#
# The command runs from rust/ because the linker-script path is relative to it.
# ---------------------------------------------------------------------------
set(_rust_stamp "${CMAKE_BINARY_DIR}/rust/.stamp")
set(_rust_rustflags "-C relocation-model=static -C link-arg=-T../apps/app.ld")

# ---------------------------------------------------------------------------
# HOST build-script linker (Windows only).
#
# The build scripts (slint-build, proc-macro2, ...) are HOST programs and need
# a native Windows linker. rustc defaults to MSVC's link.exe — but this project
# deliberately does not require MSVC, and under ./build.sh the default is
# actively broken: build.sh puts MSYS2's usr/bin first, where `link.exe` is
# coreutils `link` (a hardlink tool), so every build script dies with
#
#     /usr/bin/link: extra operand '....rcgu.o'
#
# MSYS2's clang64 ships lld-link.exe, which links these host binaries with no
# MSVC install at all: it resolves its own import libraries (ws2_32, dbghelp,
# ...) with no LIB variable set. usr/bin also ships an lld-link, but that copy
# is a Cygwin binary and does NOT find them — hence the clang64 copy is chosen
# deliberately rather than by bare name, which PATH order could redirect.
#
# The root is derived from KYUZEN_CLANG (<root>/usr/bin/clang) instead of being
# hardcoded, so the tree keeps working wherever MSYS2 lives.
#
# CMAKE_HOST_WIN32, not WIN32: this project's toolchain sets
# CMAKE_SYSTEM_NAME=Generic (bare-metal target), so WIN32 is FALSE even on a
# Windows host. The question here is about the HOST, not the target.
#
# On Linux this block is inert and the native linker is used as usual.
# ---------------------------------------------------------------------------
set(_rust_host_linker_env "")
if(CMAKE_HOST_WIN32)
    get_filename_component(_clang_bin "${KYUZEN_CLANG}" DIRECTORY)   # <root>/usr/bin
    get_filename_component(_clang_usr "${_clang_bin}" DIRECTORY)    # <root>/usr
    get_filename_component(_msys_root "${_clang_usr}" DIRECTORY)    # <root>

    find_program(KYUZEN_LLD_LINK
        NAMES lld-link lld-link.exe
        HINTS "${_msys_root}/clang64/bin"
        NO_DEFAULT_PATH
    )
    if(NOT KYUZEN_LLD_LINK)
        # Fall back to a PATH lookup, but only accept it if it is NOT the
        # Cygwin copy in usr/bin (that one cannot find the import libraries).
        find_program(KYUZEN_LLD_LINK NAMES lld-link lld-link.exe)
        if(KYUZEN_LLD_LINK AND KYUZEN_LLD_LINK MATCHES "/usr/bin/")
            set(KYUZEN_LLD_LINK "KYUZEN_LLD_LINK-NOTFOUND")
        endif()
    endif()

    if(KYUZEN_LLD_LINK)
        message(STATUS "rust: host build-script linker: ${KYUZEN_LLD_LINK}")
        set(_rust_host_linker_env
            "CARGO_TARGET_X86_64_PC_WINDOWS_MSVC_LINKER=${KYUZEN_LLD_LINK}")
    else()
        message(WARNING
            "rust: clang64 lld-link.exe not found under ${_msys_root}/clang64/bin.\n"
            "  The host build scripts would use rustc's default linker, which "
            "under ./build.sh resolves to MSYS2 coreutils `link` and fails with "
            "'extra operand'.\n"
            "  Install it with: pacman -S mingw-w64-clang-x86_64-lld")
    endif()
endif()

add_custom_command(
    OUTPUT "${_rust_stamp}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${KYUZEN_ELF_DIR}"
    COMMAND ${CMAKE_COMMAND} -E env "RUSTFLAGS=${_rust_rustflags}"
            ${_rust_host_linker_env}
            "${KYUZEN_CARGO}" build --release
    COMMAND ${CMAKE_COMMAND}
            -DSRC_DIR=${_rust_out}
            -DDST_DIR=${KYUZEN_ELF_DIR}
            "-DNAMES=${_rust_names}"
            -P ${KYUZEN_ROOT}/cmake/copy_rust_elfs.cmake
    COMMAND ${CMAKE_COMMAND} -E touch "${_rust_stamp}"
    WORKING_DIRECTORY "${_rust_dir}"
    COMMENT "Building Rust applications (cargo --release)"
    VERBATIM
)

add_custom_target(kyuzen-rust-apps DEPENDS "${_rust_stamp}")

# Convenience: the individual ELF paths, so the ISO can depend on them.
set(_rust_elfs "")
foreach(_n IN LISTS _rust_names)
    list(APPEND _rust_elfs "${KYUZEN_ELF_DIR}/${_n}.elf")
endforeach()

# ---------------------------------------------------------------------------
# Clean target: `cmake --build build --target rust-clean`
# ---------------------------------------------------------------------------
add_custom_target(rust-clean
    COMMAND "${KYUZEN_CARGO}" clean
    WORKING_DIRECTORY "${_rust_dir}"
    COMMENT "Cleaning the Rust workspace"
    VERBATIM
)

set(KYUZEN_RUST_ELFS "${_rust_elfs}" PARENT_SCOPE)
set(KYUZEN_RUST_TARGET "kyuzen-rust-apps" PARENT_SCOPE)
