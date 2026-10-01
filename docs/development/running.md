# Running

This document covers running KyuzenOS in QEMU and the first-boot flow.

## Quick Start

```sh
make            # build the kernel
make apps       # build the applications
make run        # build the ISO and boot QEMU
```

## Run Targets

| Target | QEMU configuration |
| --- | --- |
| `make run` | 8 CPUs, 1 GB RAM, e1000 NIC, virtio-vga 1920×1080, GTK windowed, serial to `serial.log` |
| `make run-serial` | 4 CPUs, serial to stdout |
| `make run-wd` | 4 CPUs, serial to `serial.log` |

All three build `boot_image.iso` first, then launch QEMU.

### Display options

| Variable | Effect |
| --- | --- |
| `QEMU_DISPLAY=gtk,zoom-to-fit=on` | Default display |
| `QEMU_DISPLAY=none` | Headless |
| `FULLSCREEN=0` | Windowed instead of full-screen |

Examples:

```sh
make run FULLSCREEN=0
make run QEMU_DISPLAY=none FULLSCREEN=0
```

### QEMU configuration

The default `run` command:

```text
qemu-system-x86_64 -cpu max -m 1G -boot d -smp 8 \
    -drive file=disk.img,format=raw,index=0,media=disk \
    -drive file=build/boot_image.iso,media=cdrom,index=2 \
    -nic user,model=e1000 \
    -vga none -device virtio-vga,xres=1920,yres=1080 \
    -display gtk,zoom-to-fit=on -serial file:serial.log
```

COM1 is always directed to `serial.log`, so a panic leaves a trace even if the
window is not visible.

## Disk Image

The kernel mounts a KyuzenFS disk image (`disk.img`) at boot. If the image is
absent, create one (about 100 MB):

```sh
make mkfs
./mkfs.kyuzenfs disk.img
```

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

### `clang: command not found`

- Ensure the MSYS2 UCRT64 `bin` directory is on `PATH`.
- On Linux, install clang (version 14 or newer).

### `nasm: command not found`

- Windows: `pacman -S mingw-w64-ucrt-x86_64-nasm`
- Linux: `sudo apt install nasm`

### No QEMU display

- Linux: install GTK3 libraries, or use `QEMU_DISPLAY=sdl`.
- Windows: ensure a GTK display is available, or use `-display none` for
  headless mode.

### Rust applications do not build

- Run `rustup target add x86_64-unknown-none`.
- Check that `rust/.cargo/config.toml` exists and the linker is correct.

### A build recipe fails

- Run `make clean` and rebuild.
- Inspect `serial.log` for QEMU-side errors.

## Related Documentation

- [Building](building.md)
- [Testing](testing.md)
- [Debugging](debugging.md)
- [Shell & CLI](../userspace/shell.md)
