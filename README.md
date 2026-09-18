<div align="center">
  <img src="assets/logo/logo.png" alt="Kyuzen OS Logo" width="200" height="200" />

  # Kyuzen OS

  **A 64-bit Higher-Half Operating System Built from Scratch - with a real GUI, SMP, and protected user space**

  ![Arch](https://img.shields.io/badge/arch-x86__64-blue)
  ![Compiler](https://img.shields.io/badge/compiler-clang-orange)
  ![Bootloader](https://img.shields.io/badge/bootloader-limine-lightgrey)
  ![Format](https://img.shields.io/badge/executable-ELF64-yellow)
  ![License](https://img.shields.io/badge/license-MIT-green)
  ![Lang](https://img.shields.io/badge/lang-C%20%2B%20NASM%20ASM-informational)
  ![Runs on](https://img.shields.io/badge/runs%20on-bare%20metal%20%2F%20QEMU-success)

  <img src="DOCUMENTATION/screenshots/desktop.png" alt="Kyuzen OS Desktop - Calculator, File Manager, and terminal running concurrently" width="85%" />

  <i>Concurrent GUI apps (spawned as independent Ring-3 tasks) over a composited desktop - Calculator &amp; File Manager side by side.</i>
</div>

---

## 📑 Table of Contents
- [About](#-about)
- [Feature Highlights](#-feature-highlights)
- [Bundled Applications](#-bundled-applications)
- [Getting Started](#-getting-started)
- [First Boot](#-first-boot)
- [Testing & Debugging](#-testing--debugging)
- [Directory Structure](#-directory-structure)
- [Documentation](#-documentation)
- [Roadmap Status](#-roadmap-status)
- [License](#-license)

## 🧭 About

Kyuzen OS is a monolithic operating system written from scratch in C and NASM assembly - no standard library, no starter code. It boots on bare metal (BIOS & UEFI) into a higher-half 64-bit kernel with preemptive multi-core scheduling, memory-protected Ring-3 applications, a composited window manager, its own filesystem, and a TCP/IP stack.

Everything on screen - every pixel, window, and keystroke - is produced by code in this repository.

## ✨ Feature Highlights

<table>
<tr>
<td width="50%" valign="top">

### 🖥️ Kernel & CPU
- 64-bit higher-half kernel (`0xFFFFFFFF80000000+`), Limine boot protocol (BIOS + UEFI hybrid ISO)
- **SMP**: multi-core boot via LAPIC, per-CPU run queues with **work stealing**, priorities + anti-starvation aging
- **Preemptive scheduler** with full-ISR-frame context switch; per-task kernel stacks (**RSP0 follows task**)
- Sleep queues, wait queues, mutex / semaphore / condition variable

### 🛡️ Protection & Isolation
- **Ring-3 user space** with per-process address spaces (PML4 per app)
- **SMAP / SMEP + WP** enforcement; all user pointers validated & copied through a boundary layer (`copy_to_user` / `copy_from_user`)
- Per-process user heap; per-address-space cookies
- `badptr` self-test app that probes the isolation boundaries

</td>
<td width="50%" valign="top">

### 🪟 Display & GUI
- Framebuffer graphics primitives on a **DisplayBuffer / Viewport** abstraction
- **Compositor**: dirty-region tracking + double buffering (no flicker, cheap repaints)
- **KWM window manager**: drag, z-order, per-task window ownership
- `libgui` framework for user apps (windows, buttons, canvas)

### ⌨️ Input & Devices
- PS/2 **keyboard**: full key down/up events, modifiers (Shift/Ctrl/Alt/CapsLock), extended `E0` scancodes
- PS/2 **mouse** with IntelliMouse scroll wheel
- Interrupt-driven event queue (ISR push → syscall pop)
- ATA PIO storage, RTC, serial, PCI, PIT/LAPIC timer (60/100/144 Hz refresh)

### 🌐 Storage & Networking
- **KyuzenFS** on a VFS layer + POSIX-ish fd API (`open/read/write/lseek/close`) — **path-aware with real folders** (`mkdir`, nested paths; folder = flagged directory entry)
- **lwIP TCP/IP** on an Intel e1000 NIC: DHCP, DNS, ICMP **ping**, TCP client sockets

</td>
</tr>
</table>

## 📦 Bundled Applications

| App | Description |
| --- | --- |
| `shell` | Login shell with 25+ commands (`help`, `ls`, `zen`, `start`, `sched`, `ping`, …) |
| `fileman` | File manager for KyuzenFS |
| `viewer` | PNG image viewer (stb_image) |
| `clock` | Real-time clock widget |
| `calc` | Calculator |
| `taskmgr` | Task / system monitor |
| `notepad` | Text editor |
| `badptr` | Ring-3 isolation self-test |

App binaries ship as `.elf` files (plus optional `<name>.app` manifests) stored under **`/apps/`** on the KyuzenFS disk. The desktop launcher scans `/apps`, and the ELF loader resolves bare names — `clock`, `start calc` — to `/apps/<name>.elf` automatically. User data (`*.txt`, `*.png`, `users.sys`) stays at the root.

<details>
<summary><b>💬 Shell quick tour (click to expand)</b></summary>

```text
root@kyuzen> help            # list every command
root@kyuzen> ls              # KyuzenFS listing
root@kyuzen> zen notes.txt   # text editor
root@kyuzen> start clock     # spawn a CONCURRENT GUI app (new Ring-3 task)
root@kyuzen> sched           # scheduler/CPU dump (tasks, per-CPU map)
root@kyuzen> fetch           # neofetch-style system info
root@kyuzen> ping 8.8.8.8    # ICMP echo over lwIP
root@kyuzen> nettest 10.0.2.2 7777   # TCP socket test
root@kyuzen> refresh 144     # set display refresh rate
```

Typing any app name (`clock`, `calc`, `fileman`…) execs it in place;
`start <app>` spawns it **concurrently** - the shell keeps running.
</details>

## 🚀 Getting Started

### Prerequisites
- **Clang/LLVM** (`clang`, `ld.lld`)
- **NASM**
- **GNU Make**
- **xorriso** (hybrid ISO)
- **QEMU** (`qemu-system-x86_64`)

### Build & Run
```bash
make                # 1. compile kernel → build/myos.bin
make apps           # 2. compile user apps → build/*.elf
make boot_image.iso # 3. package hybrid BIOS+UEFI ISO → build/boot_image.iso
make run            # 4. boot in QEMU (-cpu max -m 1G -smp 4)
```

<details>
<summary><b>🛠️ All build targets (click to expand)</b></summary>

| Target | Purpose |
| --- | --- |
| `make` / `make all` | Compile kernel (`build/myos.bin`) |
| `make apps` | Compile all user apps (`build/*.elf`) |
| `make boot_image.iso` | Kernel + apps + Limine → bootable ISO (`build/boot_image.iso`) |
| `make run` | Build & boot QEMU with a virtual disk |
| `make stress` | PMM stress test build + run |
| `make conc` | Concurrency test build + run (mutex/sem/condvar) |
| `make heap-stress` | Heap overflow/canary detection build |
| `make heap-watch` | Heap watch debug build (serial logging) |
| `make clean` / `make clean-apps` | Remove kernel / user-app objects |
| `make compile_commands` | Regenerate IntelliSense database |
</details>

## 🔑 First Boot

On a fresh disk, Kyuzen runs a one-time setup asking you to **create the root password** (stored in `users.sys`). Afterwards you land on the login screen; logging in drops you into the shell.

## 🧪 Testing & Debugging

- **`make conc`** - sleep/wait-queue/mutex/semaphore/priority test suite (`-smp 4`)
- **`make stress`** - physical memory manager stress
- **`make heap-stress` / `make heap-watch`** - heap canary + watchpoint corruption hunting
- **`badptr`** user app - attacks the Ring-3 boundary (null/unmapped pointers, invalid syscall args)
- **BOSD** (Blue Screen of Death) exception screen with full register dump, CR2, task & CPU context

## 📂 Directory Structure

| Directory | Main Function |
| --- | --- |
| `arch/x86/` | Architecture code: GDT/TSS, IDT, ISR/LAPIC/SMP entry (ASM) |
| `kernel/` | Core: PMM, VMM/paging, heap, ELF loader, syscalls, event queue |
| `kernel/sched/` | Scheduler: run queues, lifecycle, blocking/sleep |
| `kernel/smp/` | Multi-core bring-up |
| `kernel/gfx/` | Compositor, KWM window manager, framebuffer |
| `drivers/` | ATA, keyboard, mouse, PCI, RTC, serial, timer, TTY |
| `drivers/net/` | e1000 NIC driver + lwIP port |
| `fs/` | VFS abstraction + fd layer |
| `kernel/kyuzenfs.c` | KyuzenFS implementation (path-aware, folder support, `/apps/`) |
| `apps/` | Kernel-side apps (shell, login) & userlib shims |
| `user_apps/` | Ring-3 ELF applications (fileman, clock, calc, …) |
| `include/` | Global headers |
| `third_party/net/lwip/` | lwIP TCP/IP stack (vendored) |
| `DOCUMENTATION/` | Design docs, troubleshooting post-mortems, screenshots |
| `limine/` | Pre-built bootloader binaries |
| `build/` | Build output (gitignored): `myos.bin`, `*.elf` apps, ISO, `iso_root/` staging |

## 📚 Documentation

- [`DOCUMENTATION.md`](DOCUMENTATION.md) - main technical reference (subsystems, APIs, syscalls)
- [`DOCUMENTATION/design/`](DOCUMENTATION/design/) - design decisions per milestone (Ring-3 stages, scheduler split, GUI phases)

## 🗺️ Roadmap Status

| Phase | Scope | Status |
| --- | --- | --- |
| 1–2 | Framebuffer & graphics primitives | ✅ |
| 3 | Display System (DisplayBuffer, compositor, dirty region, viewport, terminal scrollback) | ✅ |
| 4 | Input Subsystem (keyboard events + modifiers, mouse + wheel, interrupt-driven queue) | ✅ |
| 5 | Window Manager - 5A concurrent spawn ✅ · routing/focus/decorations **in progress** | 🚧 |
| 6+ | GUI framework, widgets, desktop environment | ⏳ |
| FS 1–3 | KyuzenFS directories — path-aware FS, `mkdir`, apps migrated to `/apps/` | ✅ |

## 📄 License

Distributed under the **MIT License** - free to use, modify, and redistribute. See [`LICENSE`](LICENSE).

<br/>
<div align="center">
  <i>Built with a modern Clang toolchain (<code>-mno-red-zone</code>, <code>-mcmodel=kernel</code>, <code>-ffreestanding</code>) for stable kernel-level performance.</i>
</div>
