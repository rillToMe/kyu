# Boot Process

KyuzenOS boots through the Limine bootloader, which loads the kernel ELF into
the higher half, installs a direct physical map, and hands control to
`kernel_main()` along with the firmware memory map and framebuffer
information.

## Boot Chain Overview

```text
Firmware (SeaBIOS or UEFI)
    │
    ▼
Limine bootloader
    ├─ parses limine.conf
    ├─ loads the kernel ELF at KERNEL_VMA (0xFFFFFFFF80000000)
    ├─ builds the Higher-Half Direct Map (HHDM)
    ├─ loads all module_path entries (apps, assets, fonts) into memory
    ├─ discovers the boot framebuffer (1920×1080 requested)
    └─ jumps to kernel entry point (kernel_main)
            │
            ▼
kernel_main()  — early initialization, in order
```

## Limine Configuration

The boot entry is defined in [`limine.conf`](../../limine.conf):

| Setting | Value |
| --- | --- |
| Protocol | `limine` |
| Boot timeout | 1 second |
| Kernel path | `boot():/myos.bin` |
| Requested resolution | `1920x1080` |

The kernel requests a 32-bit-per-pixel XRGB8888 framebuffer. The kernel
validates the framebuffer description in `display_boot_init()` and halts if it
is not 32 bpp, RGB memory model, with 8/8/8 channel masks. Runtime mode-setting
is not supported by the software and Limine backends; the resolution is fixed
at boot.

### Modules

`limine.conf` declares roughly 78 module entries. Every module is installed
into the filesystem at boot by the kernel's module loop, which routes files by
name:

- `*.elf` and `*.app` → `/apps/`
- Everything else (icons, wallpapers, fonts, `neofect.json`) → filesystem root

Modules are listed with flat paths (`boot():/<basename>`); the kernel extracts
the basename after the last `/`. See
[Filesystem](../filesystem/README.md) for the directory layout created at boot.

## Kernel Linker Script

[`linker.ld`](../../linker.ld) defines the kernel image layout:

| Symbol / section | Purpose |
| --- | --- |
| `KERNEL_VMA = 0xFFFFFFFF80000000` | Virtual base address of the kernel |
| `.text` | Executable code |
| `.rodata` | Read-only data |
| `.requests` | **Limine request structures** — kept verbatim so the bootloader can find memory-map, HHDM, and framebuffer requests |
| `.data` | Initialized data |
| `.bss` | Zero-initialized data |

The entry point is `kernel_main`. The `.requests` section must not be
reordered or garbage-collected, or Limine cannot locate the kernel's requests.

## Initialization Order

`kernel_main()` performs initialization in a deliberate order because later
stages depend on earlier ones:

1. **Console / serial** — early logging is available before anything else.
2. **Limine request processing** — retrieve memory map, HHDM offset, and
   framebuffer description.
3. **Physical memory manager (PMM)** — parse the memory map and build the
   page-frame allocator. See [Memory Management](../kernel/memory.md).
4. **Virtual memory manager (VMM)** — enable paging structures, map the
   higher-half direct map.
5. **Kernel heap** — initialize the kernel's `kmalloc`/`kfree` allocator,
   which all later allocations depend on.
6. **CPU setup** — GDT and TSS (including per-CPU TSS entries), IDT, then
   SMAP/SMEP and WP enforcement. See
   [Interrupts & Timers](../kernel/interrupts.md).
7. **Drivers** — PCI enumeration, ATA storage, PS/2 keyboard and mouse, RTC,
   serial, PIT timer.
8. **Subsystems** — filesystem mount, network stack initialization, graphics
   HAL initialization, KWM/compositor setup.
9. **SMP bring-up** — start application processors via LAPIC. Each AP runs its
   own init sequence (GDT/TSS/IDT, SMAP/SMEP, LAPIC timer) and joins the
   scheduler.
10. **Tasking initialization** — `tasking_init()` registers the kernel main
    thread as **Task 0**.
11. **Module installation** — install Limine modules into the filesystem and
    create the standard directory tree.
12. **Userspace handoff** — start the desktop environment or login flow.

> **Ordering constraint:** `tasking_init()` must be called after the timer is
> initialized and before any `create_task()`. The kernel heap must exist before
> the VMM can allocate page tables for user address spaces.

## Framebuffer Handoff

Limine provides a linear framebuffer. The kernel:

1. Validates the format (`display_boot_init`) — 32 bpp, RGB, 8/8/8.
2. Records the geometry in a single `display_mode_t` (the source of truth for
   screen dimensions).
3. Allocates the base canvas and back buffer once the heap is ready
   (`display_alloc_buffers`).
4. Passes the framebuffer to the graphics HAL, which selects a backend.

See [Graphics](../graphics/README.md) for the backend selection order.

## Module Installation

The kernel installs each Limine module into the KyuzenFS filesystem. A standard
directory tree is created if absent:

```text
/apps          — application ELF files and manifests
/system        — system configuration
/system/config
/system/fonts
/home
/home/user
/home/user/{Documents,Downloads,Pictures,Projects}
```

Application names without a path separator resolve to `/apps/<name>`; see the
ELF loader in [Process Model](../kernel/processes.md).

## Related Documentation

- [Architecture Overview](overview.md)
- [Memory Management](../kernel/memory.md)
- [Interrupts & Timers](../kernel/interrupts.md)
- [Building](../development/building.md)
- [Running](../development/running.md)
