# Rust Support

KyuzenOS provides an optional Rust userspace toolchain for building `no_std`
applications. Rust is not required to build the kernel or the C/C++ SDK.

## Workspace Layout

```text
rust/
├── Cargo.toml          # workspace
├── kyuzen-sys/         # raw syscall bindings
├── kyuzen-alloc/       # custom global allocator
├── kyuzen-gui/         # Slint platform adapter
└── apps/
    ├── hello-slint/    # example
    └── control-center/ # example
```

The workspace keeps Cargo metadata and `target/` out of the C/C++ build tree.

## Crates

| Crate | Purpose |
| --- | --- |
| `kyuzen-sys` | Raw syscall bindings (the Rust equivalent of `userlib`) |
| `kyuzen-alloc` | A global allocator backed by the kernel user heap |
| `kyuzen-gui` | A Slint `Platform`/`WindowAdapter` adapter bridging Slint onto the Kyuzen userspace ABI (KWM window syscalls + the per-task event queue) |

## Configuration

- **Target**: `x86_64-unknown-none`.
- **Profile**: `panic = "abort"` — Kyuzen loads ELFs without unwinding support.
- **Slint feature set**: the embedded/MCU profile (`default-features = false`,
  `compat-1-2`, `unsafe-single-threaded`).

## Building

```sh
rustup target add x86_64-unknown-none
./build.sh kyuzen-rust-apps   # cargo build --release, then copy ELFs to build/target/apps/
```

The resulting ELFs (`hello-slint.elf`, `control-center.elf`) are installed to
`/apps/` at boot.

### Linkers (two of them, on purpose)

| Target | Linker | Configured where |
| --- | --- | --- |
| `x86_64-unknown-none` (the apps) | `rust-lld`, bundled with rustc | nowhere — rustc selects it |
| Host build scripts (`slint-build`, …) | MSYS2 clang64 `lld-link.exe` on Windows; the native linker on Linux | `rust/CMakeLists.txt` |

Neither is stored in `rust/.cargo/config.toml`, and that is deliberate. An
absolute `linker` path there is resolved against the **workspace root**, so a
machine-specific path breaks every other checkout — which is exactly what
happened before.

On Windows the host linker needs an override at all: `./build.sh` puts MSYS2's
`usr/bin` first, where `link.exe` is coreutils `link` (a hardlink tool), so every
build script fails with `extra operand`. `rust/CMakeLists.txt` locates
`clang64/bin/lld-link.exe` and passes it through
`CARGO_TARGET_X86_64_PC_WINDOWS_MSVC_LINKER`. **No MSVC installation is
required.** Use the clang64 copy rather than a bare `lld-link`: `usr/bin` ships a
Cygwin build that cannot find its own import libraries (`ws2_32.lib`,
`dbghelp.lib`).

Two build-time dependencies are easy to miss on Linux, because they fail inside
`slint-build` rather than at configure time:

- `python3-yaml` — needed by the nested LLVM libc build.
- a sans-serif font (`fonts-dejavu-core` / `ttf-dejavu`) — `slint-build`
  rasterises the UI at build time and cannot pick a default font without one.


## Limitations

- `no_std` only.
- Panics abort (no unwinding).
- The Slint integration uses the single-threaded MCU profile.
- Rust applications are optional; the default desktop and applications are
  C/C++.

## Related Documentation

- [Building](../development/building.md)
- [C SDK](c-sdk.md)
- [Userspace Model](../userspace/overview.md)
