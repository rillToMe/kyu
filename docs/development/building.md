# Building

This document covers toolchain setup and the build targets for KyuzenOS.

## Prerequisites

### Required tools

| Tool | Purpose | Notes |
| --- | --- | --- |
| `clang` | Compile kernel, apps, host tests | Target `x86_64-pc-none-elf` |
| `nasm` | Assemble ISR stubs, GDT/TSS | ELF64 output |
| `ld.lld` | Link the kernel and user ELFs | GNU flavor |
| `cmake` + `ninja` | Build orchestration | CMake ≥ 3.20 |
| `qemu-system-x86_64` | Run the system | |
| `xorriso` | Build the hybrid BIOS+UEFI ISO | `xorriso -as mkisofs` |
| `git` | Version control | |

### Optional tools

| Tool | Purpose |
| --- | --- |
| `rustup` + `rustc` | Build Rust applications (`./build.sh iso` needs cargo) |
| `python3` | Used by the nested LLVM libc build |

### Already in the repository

| Item | Location |
| --- | --- |
| Limine bootloader binaries | `limine/` |
| lwIP TCP/IP stack | `third_party/net/lwip/` |
| FreeType | `third_party/freetype/` |
| BearSSL | `third_party/bearssl/` |
| Kyuzen LLVM libc config | `third_party/stdlib/kyuzen/` |

### Fetched, not committed: llvm-project

The C and C++ SDKs are built from LLVM's `libc` and `libcxx`. Upstream is
~1.9 GB, so it is **not** in the repository. Fetch it once:

```sh
./build.sh setup
```

That clones it sparsely — only `libc`, `libcxx`, `libcxxabi`, `libunwind`,
`cmake`, `runtimes` and `llvm/cmake` — at the pinned tag `llvmorg-22.1.8`.
About **137 MB instead of 1.9 GB**, in roughly five minutes. The tag is verified
against the commit the parity baseline was built with.

`third_party/stdlib/kyuzen/` holds the Kyuzen-specific libc configuration
(upstream ships no x86_64 baremetal variant); `setup` copies it into the clone.

To skip the SDKs entirely and build just the kernel and non-SDK apps:

```sh
cmake -S . -B build/target -G Ninja -DKYUZEN_BUILD_LIBC_SDK=OFF
```

## Platform Setup

### Windows (MSYS2)

The project needs tools from **three** MSYS2 environments, so the package list
spans more than one. Install everything from a single shell:

```sh
pacman -Suy
# MSYS toolchain — clang 21 (the parity baseline), nasm, xorriso
pacman -S clang nasm xorriso make git python
# Native Windows toolchain — cmake and ninja
pacman -S mingw-w64-clang-x86_64-cmake mingw-w64-clang-x86_64-ninja
# QEMU
pacman -S mingw-w64-x86_64-qemu
```

You do **not** need to pick the right shell or edit PATH by hand: `./build.sh`
finds the MSYS2 root, puts `usr/bin` first (so clang 21 wins) and calls
`cmake`/`ctest` from `clang64/bin` by absolute path. See
[Why the script exists](#why-the-script-exists) for what goes wrong otherwise.

Verify with:

```sh
./build.sh setup     # toolchain report + fetch llvm-project if missing
```

On a fresh clone the first run does both: it reports the resolved MSYS2 root,
cmake path and clang version, then fetches llvm-project. Running it again is
safe — an existing clone is detected and left alone.

Optionally install Rust (`rustup target add x86_64-unknown-none`) — the `iso`
target needs `cargo` on PATH, and skips the Rust apps without it.

### Linux

```sh
# Ubuntu / Debian
sudo apt update && sudo apt install -y clang nasm lld cmake ninja-build \
    qemu-system-x86 xorriso git python3 ovmf
```

```sh
# Arch
sudo pacman -S clang nasm lld cmake ninja qemu-full xorriso git python
```

`./build.sh` is written for MSYS2 and expects `usr/bin`, `clang64/bin` and
`mingw64/bin` under one root. On Linux those paths do not exist, so configure
CMake directly instead — the two-clang problem it works around is MSYS2-specific:

```sh
cmake -S . -B build/target -G Ninja
cmake --build build/target
```

## Build Targets

Run all commands from the repository root.

> **Use `./build.sh`.** It locates the MSYS2 installation and sets PATH
> correctly no matter which shell you started in. Building with plain
> `cmake`/`ninja` works too, but needs a specific PATH setup — see
> [Why the script exists](#why-the-script-exists) below.

### Kernel

```sh
./build.sh           # → build/target/bin/myos.bin
```

This builds the kernel and its libraries. It also writes a byte-identical copy
at `build/target/myos.bin`.

### Applications

```sh
./build.sh           # kernel + libraries + every application
./build.sh kyuzen-desktop   # only desktop.elf
```

Individual targets are Ninja targets; `./build.sh <target>` forwards to Ninja.
Useful ones: `kyuzen-kernel`, `kyuzen-desktop`, `kyuzen-browser`,
`kyuzen-rust-apps`, `kyuzen-sdk-c`, `kyuzen-sdk-cpp`.

### Full ISO

```sh
./build.sh iso       # kernel + apps + Rust + assets + Limine → build/target/boot_image.iso
```

### Utilities

```sh
./build.sh clean           # remove build/ entirely (a complete reset)
./build.sh run             # boot in QEMU, serial → serial.log
./build.sh run-serial      # boot in QEMU, serial → terminal
./build.sh run-wd          # boot in QEMU, serial → serial.log (reliable on Windows)
```

## Build Architecture

```text
Sources (kernel/*.c, apps/*.cpp, libs/**, system/**)
    ↓ clang / clang++ / rustc
Objects (build/target/**/CMakeFiles/**)
    ↓ ld.lld / rust-lld
Binaries (build/target/bin/myos.bin, build/target/apps/*.elf)
    ↓ xorriso + limine
ISO (build/target/boot_image.iso)
    ↓ QEMU
Running system
```

### Output layout

Everything generated lives under `build/`, and **nothing is written into the
source tree**, so `rm -rf build` is a complete reset:

```text
build/target/        kernel + libs + apps + ISO
  bin/myos.bin       the kernel
  apps/*.elf         user-space ELFs
  iso_root/          ISO staging
  boot_image.iso     the hybrid BIOS+UEFI image
  tools/             generated compiler wrappers
build/host/          host-side unit tests (separate build tree)
```

### Why the script exists

MSYS2 ships **two** complete clang toolchains, and this project needs tools from
both — in a combination no single PATH ordering produces:

| Tool | Must come from | Why |
| --- | --- | --- |
| `clang`, `nasm`, `xorriso`, `ld.lld` | `usr/bin` | LLVM **21** — the parity baseline |
| `cmake`, `ninja`, `ctest` | `clang64/bin` | native Windows binaries |
| `qemu-system-x86_64` | `mingw64/bin` | — |

Getting this wrong fails in two very different ways:

- **`/usr/bin/cmake`** is a Cygwin binary. It writes POSIX paths into the Ninja
  files it generates, and native Ninja cannot resolve them — the build dies with
  `'.../CMakeScratch/TryCompile-.../testCCompiler.c' ... missing and no known
  rule to make it`, which looks like a broken source tree.
- **`clang64/bin/clang`** is LLVM 22. It builds successfully and produces a
  **byte-different kernel** with no error at all, silently breaking parity with
  the verified baseline.

`./build.sh` puts `usr/bin` first and invokes `cmake`/`ctest` by absolute path
from `clang64/bin`. Doing it by hand requires both:

```sh
export PATH="$MSYS_ROOT/usr/bin:$MSYS_ROOT/clang64/bin:$MSYS_ROOT/mingw64/bin:$PATH"
"$MSYS_ROOT/clang64/bin/cmake" -S . -B build/target -G Ninja
"$MSYS_ROOT/clang64/bin/cmake" --build build/target
```

### Object layout

All build output is under `build/` (gitignored):

- `build/obj/<path>.o` — kernel objects, mirroring the source tree.
- `build/obj/user/<path>.o` — userspace objects in a **separate namespace**.
  Files such as `libs/core/userlib.c` are compiled twice (kernel and user) with
  different flags.
- `build/bin/myos.bin`, `build/apps/*.elf`, `build/sdk/`, `build/iso_root/`.

The kernel source directories are listed explicitly in `SRC_DIRS` (not a
recursive `find`), so `third_party/`, `tests/`, and `rust/target` are never
swept into the kernel build.

### Key compiler flags

Kernel:

```text
--target=x86_64-pc-none-elf -ffreestanding -O2 -nostdlib -mcmodel=kernel
-mno-red-zone -mno-sse -mno-sse2 -mno-mmx -msoft-float
```

- `-mcmodel=kernel` is mandatory: higher-half symbols exceed 4 GB.
- `-mno-sse*` and `-msoft-float` are project-wide; the kernel never enables
  SSE, and produced ELFs are scanned for SSE/x87 instructions and rejected.

## IntelliSense / `compile_commands.json`

CMake generates a compilation database automatically — no extra tooling:

| Database | Covers |
| --- | --- |
| `build/target/compile_commands.json` | the target build (kernel, libs, apps) |
| `build/host/compile_commands.json` | the host tests |

Point `.vscode/c_cpp_properties.json` at whichever one you are working in. They
describe **different flag sets** (bare-metal vs host) and are not
interchangeable — an IDE pointed at the wrong one shows phantom errors on every
include.

Both are rewritten on every configure, so they stay current when you add a
source file or change an include path. `compile_commands.json` and `.vscode/`
are machine-specific and gitignored.

## Related Documentation

- [Running](running.md)
- [Testing](testing.md)
- [Debugging](debugging.md)
- [C SDK](../libraries/c-sdk.md), [C++ SDK](../libraries/cpp-sdk.md)
