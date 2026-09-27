# Kyuzen-OS — Setup & Run Guide

## Table of Contents

- [Ringkasan](#ringkasan)
- [Yang Dibutuhkan](#yang-dibutuhkan)
- [Instalasi per Platform](#instalasi-per-platform)
  - [Windows (MSYS2)](#windows-msys2)
  - [Linux (Ubuntu/Debian)](#linux-ubuntudebian)
  - [Linux (Arch)](#linux-arch)
- [Build & Run](#build--run)
- [Aplikasi](#aplikasi)
- [Troubleshooting](#troubleshooting)

---

## Ringkasan

Kyuzen-OS adalah kernel 64-bit higher-half monolithic. Build menghasilkan ISO bootable yang dijalankan di QEMU.

**Minimum:** `clang` + `nasm` + `ld.lld` + `qemu-system-x86_64` + `xorriso` + `git`

---

## Yang Dibutuhkan

### Toolchain Wajib

| Tool | Fungsi | Catatan |
|------|--------|---------|
| `clang` | Compiler C/C++ (kernel + user apps) | Target: `x86_64-pc-none-elf` |
| `nasm` | Assembler (ISR stubs, GDT/TSS) | Output: ELF64 |
| `ld.lld` | Linker (kernel + apps) | GNU flavor |
| `qemu-system-x86_64` | Emulator | Virtualisasi hardware |
| `xorriso` | Pembuat ISO hybrid BIOS+UEFI | `xorriso -as mkisofs` |
| `git` | Version control | Clone repo |

### Toolchain Opsional

| Tool | Fungsi |
|------|--------|
| `rustup` + `rustc stable` | Build Rust apps (`hello-slint`, `control-center`) |
| `cmake` | Cross-compile LLVM libc (SDK C++) |
| `python` + `pip install compiledb` | Generate `compile_commands.json` untuk IDE |
| `make` | Build system |

### Yang Sudah Ada di Repo (Tidak Perlu Install)

| Item | Lokasi |
|------|--------|
| Limine bootloader | `limine/` (pre-built binaries + `limine.exe`) |
| lwIP network stack | `third_party/net/lwip/` |
| LLVM libc + libcxx (cross-compile source) | `third_party/stdlib/llvm-project/` |
| Widget toolkit (GUI) | `libs/widget/` |
| Color library | `libs/color/` |
| libdesktop framework | `libs/libdesktop/` |

---

## Instalasi per Platform

### Windows (MSYS2)

1. **Install MSYS2** → https://www.msys2.org/
   Jalankan installer, pilih folder `C:\msys2` (atau `E:\Tools\msys2`).

2. **Buka MSYS2 UCRT64 terminal** (bukan MINGW64 atau MSYS).

3. **Install packages:**

   ```sh
   pacman -Suy
   pacman -S mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-nasm \
             mingw-w64-ucrt-x86_64-lld mingw-w64-ucrt-x86_64-cmake \
             mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-qemu \
             mingw-w64-ucrt-x86_64-xorriso mingw-w64-ucrt-x86_64-python-pip \
             make git
   ```

4. **Tambahkan ke PATH** (jika belum otomatis):
   Edit environment variables Windows → tambah:
   ```
   C:\msys2\ucrt64\bin
   ```
   Atau untuk instalasi custom di `E:\Tools\msys2`:
   ```
   E:\Tools\msys2\ucrt64\bin
   ```

5. **Install compiledb** (opsional, untuk IntelliSense):
   ```sh
   pip install compiledb
   ```

6. **Install Rust** (opsional, untuk Rust apps):
   ```sh
   wingup install rustup-init
   rustup target add x86_64-unknown-none
   ```

7. **Set环境变数 di PowerShell/Command Prompt:**
   ```powershell
   # Tambahkan sementara (per session)
   $env:PATH = "E:\Tools\msys2\ucrt64\bin;$env:PATH"

   # Atau permanen via System Properties → Environment Variables
   ```

### Linux (Ubuntu/Debian)

1. **Update & install packages:**

   ```sh
   sudo apt update && sudo apt upgrade -y

   sudo apt install -y clang nasm lld qemu-system-x86 \
       xorriso git make cmake python3 python3-pip \
       ovmf
   ```

2. **Pastikan versi clang cukup baru:**
   ```sh
   clang --version   # ≥ 14 recommended, 17+ ideal
   ```

3. **Install compiledb** (opsional):
   ```sh
   pip3 install compiledb
   ```

4. **Install Rust** (opsional):
   ```sh
   curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
   rustup target add x86_64-unknown-none
   ```

### Linux (Arch)

```sh
sudo pacman -S clang nasm lld qemu-full xorriso git make cmake python python-pip
```

Rust: `curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh && rustup target add x86_64-unknown-none`

---

## Build & Run

Semua command dijalankan dari root directory repo.

### Build Kernel Saja

```sh
make              # → build/bin/myos.bin
```

### Build Semua Apps (C + C++ + Rust)

```sh
make apps         # C & C++ user ELFs → build/apps/*.elf
make rust-apps    # Rust apps → rust/target/...
```

### Build Full ISO

```sh
make boot_image.iso   # kernel + apps + Rust → build/boot_image.iso
```

### Build + Run di QEMU

```sh
make run              # Build ISO → boot QEMU (windowed)
make run-serial       # Same, COM1 di stdout
make run-wd           # Same, COM1 → serial.log
```

**QEMU Default Config:** 8 CPU, 1GB RAM, e1000 NIC, display 1920×1080

### Format Disk (opsional, sebelum first boot)

```sh
make mkfs
./mkfs.kyuzenfs disk.img    # Format disk image untuk KyuzenFS
```

Disk image (`disk.img`) 100MB akan di-mount sebagai filesystem di boot pertama.

### Login

```
User: root
Pass: 1
```

### Test Targets

| Target | Deskripsi | Catatan |
|--------|-----------|---------|
| `make test-kyuzenfs-v4` | KyuzenFS (format/CRUD/extents) | Host-side, no QEMU |
| `make test-panic` | BSOD handler | Host-side |
| `make test-ata` | ATA driver | Host-side |
| `make test-textedit` | TextEdit widget | Host-side |
| `make test-desktop` | Desktop launcher | Host-side |
| `make stress` | PMM stress test | In-QEMU |
| `make conc` | Concurrency (sleep/mutex/sem) | In-QEMU |
| `make heap-stress` | Heap overflow/canary | In-QEMU |
| `make test-libc-heap` | LLVM libc heap | Host-side |

### Utility

```sh
make compile_commands  # Regenerate compile_commands.json (IntelliSense)
make clean            # Hapus semua build artifacts
```

---

## Aplikasi

### C User Apps (built via `user_apps/`)

**GUI Apps** (link widget toolkit + PNG + color):
| App | Fungsi |
|-----|--------|
| `fileman` | File manager |
| `viewer` | Image/text viewer |
| `clock` | Jam |
| `calc` | Kalkulator |
| `taskmgr` | Task manager |
| `notepad` | Text editor |
| `settings` | Pengaturan sistem |
| `widget_demo` | Demo widget |

**Terminal Apps** (shell engine):
| App | Fungsi |
|-----|--------|
| `terminal` | Terminal emulator (menggunakan TextEdit) |

**Utility Apps** (link userlib only):
| App | Fungsi |
|-----|--------|
| `procinfo` | Process info |
| `echo` | Echo text |
| `cat` | Tampilkan file |
| `pipe_test` | Pipe test |
| `fork_test` | Fork test |
| `exit_test` | Exit test |
| `kill_test` | Kill test |
| `fd_test` | File descriptor test |
| `badptr` | Invalid pointer test (panic test) |

### C++ Apps

| App | Lokasi |
|-----|--------|
| `desktop` | `apps/desktop/` (launcher, taskbar, wallpaper, crash_notice) |
| `test-desktop` | `apps/test-desktop/` (test implementation) |

### Rust Apps

| App | Crate |
|-----|-------|
| `hello-slint` | `rust/apps/hello-slint/` |
| `control-center` | `rust/apps/control-center/` |

### Kernel Apps (compiled as Ring 0)

Lokasi: `apps/` — `shell.c`, `login.c`, `zen.c`, `shell_core.c`, dll.

### App Manifests

21 file `.app` di `manifests/` — format key=value (`name`, `color`, `icon`, `wallpaper`). Desktop launcher scan `/apps` untuk `*.elf` + `*.app`.

---

## Troubleshooting

### QEMU tidak bisa boot

- Pastikan `disk.img` ada di root repo. Jika belum: `dd if=/dev/zero of=disk.img bs=1M count=100` (Linux) atau buat via QEMU/Python.
- Pastikan `xorriso` dan `limine/` binaries ada.

### Build error: "clang: command not found"

- Pastikan MSYS2 UCRT64 bin di PATH.
- Linux: `sudo apt install clang` atau pastikan versi ≥ 14.

### Build error: "nasm: command not found"

- Windows: `pacman -S mingw-w64-ucrt-x86_64-nasm`
- Linux: `sudo apt install nasm`

### QEMU display tidak muncul

- Linux: install `gtk3` libs, atau gunakan `QEMU_DISPLAY=sdl make run`.
- Windows: pastikan GTK display tersedia, atau gunakan `-display none` untuk headless.

### Rust apps tidak build

- Pastikan `rustup target add x86_64-unknown-none` sudah dijalankan.
- Cek `rust/.cargo/config.toml` ada dan linkernya benar.

### Make error: "recipe for target '...' failed"

- Jalankan `make clean` lalu build ulang.
- Cek `serial.log` untuk error detail dari QEMU.

---

## Arsitektur Build Pipeline

```
Source (kernel/*.c, apps/*.cpp, user_apps/*.c, rust/apps/*.rs)
  ↓ clang / clang++ / rustc
Object (build/obj/...)
  ↓ ld.lld / rust-lld
Binary (build/bin/myos.bin, build/apps/*.elf)
  ↓ xorriso + limine
ISO (build/boot_image.iso)
  ↓ QEMU
Running OS
```
