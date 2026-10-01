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

add_custom_command(
    OUTPUT "${_rust_stamp}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${KYUZEN_ELF_DIR}"
    COMMAND ${CMAKE_COMMAND} -E env "RUSTFLAGS=${_rust_rustflags}"
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
