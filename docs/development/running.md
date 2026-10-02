# Running

This document covers running KyuzenOS in QEMU and the first-boot flow.

## Quick Start

```sh
./build.sh run      # build the ISO (kernel + apps + Rust) and boot QEMU
```

## Run Targets

| Target | QEMU configuration |
| --- | --- |
| `./build.sh run` | 8 CPUs, 1 GB RAM, e1000 NIC, virtio-vga 1920×1080, GTK windowed, serial to `serial.log` |
| `./build.sh run-serial` | 4 CPUs, serial to stdout |
| `./build.sh run-wd` | 4 CPUs, serial to `serial.log` |

All three build `boot_image.iso` first, then launch QEMU.

### Display options

| Option | Effect |
| --- | --- |
| `-DKYUZEN_QEMU_DISPLAY=gtk,zoom-to-fit=on` | Default display |
| `-DKYUZEN_QEMU_DISPLAY=none` | Headless |
| `-DKYUZEN_QEMU_FULLSCREEN=ON` | Full-screen instead of windowed |

These are CMake cache variables, so pass them at configure time:

```sh
cmake -S . -B build/target -G Ninja -DKYUZEN_QEMU_DISPLAY=none
./build.sh run
```

Windowed is the default. That matches what the Make build actually did: its
`Makefile` documented full-screen as the default and set `FULLSCREEN ?= 1`, but
the `FULLSCREEN_ARG` variable it defined was never referenced by any recipe, so
`make run` always launched windowed.

### QEMU configuration

The default `run` command:

```text
qemu-system-x86_64 -cpu max -m 1G -boot d -smp 8 \
    -drive file=disk.img,format=raw,index=0,media=disk \
    -drive file=build/target/boot_image.iso,media=cdrom,index=2 \
    -nic user,model=e1000 \
    -vga none -device virtio-vga,xres=1920,yres=1080 \
    -display gtk,zoom-to-fit=on -serial file:serial.log
```

COM1 is always directed to `serial.log`, so a panic leaves a trace even if the
window is not visible.

## Disk Image

The kernel mounts a KyuzenFS disk image (`disk.img`) at boot. One is already
present in the repository root, so you do not need to create one to reach the
desktop.

To build a fresh image you need the host formatter, `tools/mkfs.kyuzenfs.c`.
The Make build had a `mkfs` target for it; the CMake build exposes the same
thing as a host-tool target:

```sh
cmake --build build/host --target mkfs.kyuzenfs
./build/host/mkfs.kyuzenfs disk.img          # 100 MB default
./build/host/mkfs.kyuzenfs disk.img 256      # explicit size in MB
```

The size matters: `superblock.total_blocks` must match what
`ata_get_total_sectors()` reports when the kernel boots from the image, so
formatting an existing file preserves its current size unless you pass one
explicitly.

The disk image is mounted as the filesystem on first boot.

## First Boot

On a fresh disk, KyuzenOS runs a one-time setup asking you to **create the root
password** (stored in `users.sys`). Afterwards you land on the login screen.

Both the login screen and the shell are **ring-3 processes** (`login.elf` and
`shell.elf`, spawned by `init.elf` — PID 1). The kernel itself contains no user
code; see [Ring-3 Init Migration](../design/ring3-init-migration.md).

Default credentials:

```text
User: root
Pass: 1
```

## After Login

Logging in starts the desktop (GUI) and the console shell. `logout` ends the
shell session and returns you to the login screen.

The shell provides 30+ commands. A quick tour:

```text
root@kyuzen> help            # list every command
root@kyuzen> ls              # KyuzenFS listing
root@kyuzen> start calc      # spawn a concurrent GUI application
root@kyuzen> sched           # scheduler / CPU dump
root@kyuzen> fetch           # system information
root@kyuzen> ping 8.8.8.8    # ICMP echo over lwIP
root@kyuzen> nettest 10.0.2.2 7777   # TCP socket test
root@kyuzen> refresh 144     # set the display refresh rate
```

See [Shell & CLI](../userspace/shell.md) for the full command list.

## Troubleshooting

### QEMU does not boot

- Ensure `disk.img` exists in the repository root.
- Ensure `xorriso` and the `limine/` binaries are present.
- Run `./build.sh iso` first — `run` depends on the ISO, but a stale
  `boot_image.iso` will boot the old build.

### `./build.sh` says it cannot find MSYS2

- It walks up from its own location looking for `usr/bin/clang.exe` and
  `clang64/bin/cmake.exe`, then falls back to `/e/Tools/msys2` and `/c/msys64`.
- Point it at your install explicitly: `KYUZEN_MSYS_ROOT=/d/msys64 ./build.sh`

### `clang: command not found`

- Do not fix this by hand — run `./build.sh setup`, which reports the resolved
  MSYS2 root, cmake path and clang version.
- If clang is genuinely missing: `pacman -S clang nasm xorriso`.

### Wrong clang version

If configure prints `Wrong C compiler: found Clang 22, expected Clang 21`, the
build will succeed but produce a kernel that does **not** match the verified
baseline. `clang64/bin/clang` is LLVM 22; the project needs LLVM 21 from
`usr/bin`. `./build.sh` sets this up correctly — see
[Building](building.md#why-the-script-exists).

### `nasm: command not found`

- MSYS2: `pacman -S nasm`
- Linux: `sudo apt install nasm`

### No QEMU display

- Linux: install GTK3 libraries, or use `-DKYUZEN_QEMU_DISPLAY=sdl`.
- Windows: ensure a GTK display is available, or use `none` for headless mode.

### Rust applications do not build

- Run `rustup target add x86_64-unknown-none`.
- Check that `rust/.cargo/config.toml` exists and the linker is correct.
- Without `cargo` on PATH, the Rust apps are skipped and `./build.sh iso` fails
  at the module guard — the ISO would reference ELFs that were never built.

### A build recipe fails

- `./build.sh clean` removes `build/` entirely — a complete reset.
- Inspect `serial.log` for QEMU-side errors.

## Related Documentation

- [Building](building.md)
- [Testing](testing.md)
- [Debugging](debugging.md)
- [Shell & CLI](../userspace/shell.md)
