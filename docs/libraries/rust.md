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
make rust-apps          # cargo build --release, then copy ELFs to build/apps/
```

The resulting ELFs (`hello-slint.elf`, `control-center.elf`) are installed to
`/apps/` at boot.

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
