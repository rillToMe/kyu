# Building

This document covers toolchain setup and the build targets for KyuzenOS.

## Prerequisites

### Required tools

| Tool | Purpose | Notes |
| --- | --- | --- |
| `clang` | Compile kernel, apps, host tests | Target `x86_64-pc-none-elf` |
| `nasm` | Assemble ISR stubs, GDT/TSS | ELF64 output |
| `ld.lld` | Link the kernel and user ELFs | GNU flavor |
| `qemu-system-x86_64` | Run the system | |
| `xorriso` | Build the hybrid BIOS+UEFI ISO | `xorriso -as mkisofs` |
| `git` | Version control | |
| GNU `make` | Build orchestration | |

### Optional tools

| Tool | Purpose |
| --- | --- |
| `rustup` + `rustc` | Build Rust applications |
| `cmake` + `python3` | Cross-compile LLVM libc (the C SDK) |
| `python` + `pip install compiledb` | Generate `compile_commands.json` for IntelliSense |

### Already in the repository

| Item | Location |
| --- | --- |
| Limine bootloader binaries | `limine/` |
| lwIP TCP/IP stack | `third_party/net/lwip/` |
| LLVM libc + libc++ sources | `third_party/stdlib/llvm-project/` |
| FreeType | `third_party/freetype/` |
| BearSSL | `third_party/bearssl/` |

## Platform Setup

### Windows (MSYS2)

1. Install [MSYS2](https://www.msys2.org/) (for example, to `E:\Tools\msys2`).
2. Open the **UCRT64** terminal (not MINGW64 or MSYS).
3. Install packages:

   ```sh
   pacman -Suy
   pacman -S mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-nasm \
             mingw-w64-ucrt-x86_64-lld mingw-w64-ucrt-x86_64-cmake \
             mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-qemu \
             mingw-w64-ucrt-x86_64-xorriso mingw-w64-ucrt-x86_64-python-pip \
             make git
   ```

4. Add `E:\Tools\msys2\ucrt64\bin` to `PATH`.
5. Optionally install `compiledb` (`pip install compiledb`) and Rust
   (`rustup target add x86_64-unknown-none`).

### Linux (Ubuntu/Debian)

```sh
sudo apt update && sudo apt upgrade -y
sudo apt install -y clang nasm lld qemu-system-x86 \
    xorriso git make cmake python3 python3-pip ovmf
```

Clang 14 or newer is recommended.

### Linux (Arch)

```sh
sudo pacman -S clang nasm lld qemu-full xorriso git make cmake python python-pip
```

> **Platform note:** the root `Makefile` hardcodes the Windows binary names
> `qemu-system-x86_64.exe` and `./limine/limine.exe`. Building on Linux
> requires adjusting those names.

## Build Targets

Run all commands from the repository root.

### Kernel

```sh
make                 # → build/bin/myos.bin
```

`make` (the default target) builds only the kernel. It also writes a
byte-identical copy at `build/myos.bin`.

### Applications

```sh
make apps            # SDK (C + C++) + libdesktop + all applications
make desktop         # only desktop.elf
make rust-apps       # Rust applications
```

### Full ISO

```sh
make boot_image.iso  # kernel + apps + Rust + assets + Limine → build/boot_image.iso
```

### SDKs

```sh
make sdk-c           # stage the C SDK to build/sdk/c
make sdk-cpp         # stage the C++ SDK to build/sdk/cpp
make libdesktop      # build build/desktop/libdesktop.a
```

### Utilities

```sh
make mkfs               # build the KyuzenFS host format tool
make compile_commands   # regenerate compile_commands.json (IntelliSense)
make clean              # remove build/ and stray objects
make clean-apps         # clean only application output
```

## Build Architecture

```text
Sources (kernel/*.c, apps/*.cpp, libs/**, system/**)
    ↓ clang / clang++ / rustc
Objects (build/obj/**, build/obj/user/**)
    ↓ ld.lld / rust-lld
Binaries (build/bin/myos.bin, build/apps/*.elf)
    ↓ xorriso + limine
ISO (build/boot_image.iso)
    ↓ QEMU
Running system
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

VS Code does not read the Makefile, so IntelliSense needs a compilation
database:

1. `pip install compiledb` (once).
2. `make compile_commands` — runs a dry-run build and captures the exact flags.
3. Point `.vscode/c_cpp_properties.json` at the generated
   `compile_commands.json`.

`compile_commands.json` and `.vscode/` are machine-specific and gitignored;
regenerate them locally. You must re-run `make compile_commands` after adding a
source file or changing an include path.

## Related Documentation

- [Running](running.md)
- [Testing](testing.md)
- [Debugging](debugging.md)
- [C SDK](../libraries/c-sdk.md), [C++ SDK](../libraries/cpp-sdk.md)
