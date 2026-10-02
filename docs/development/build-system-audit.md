# KyuzenOS Build System Audit

> The Make build this project replaced, recorded before the CMake + Ninja
> migration started.
> Audit date: 2026-10-01 · Branch: `feature/64bit-migration` · Commit: `05a8d75`
>
> **This document describes a build system that no longer runs the project.**
> It is kept because it is the reference the migration's parity claims are
> measured against — every "BIT-IDENTICAL" statement in
> [CMake Migration](cmake-migration.md) is a comparison against the behaviour
> recorded here.
>
> Sections 1–13 and the appendices are the audit as written. The pre-migration
> proposal (old §14), the risk register (old §15) and the validation plan
> (old §16) have been removed: all three were forward-looking, and all three
> are now superseded by `cmake-migration.md`, which records what actually
> happened — including the risks that materialised and the ones that did not.

This document records the **actual** behaviour of the Make-based build. Every
statement below was verified by reading the build files and by executing the build.
Where behaviour was assumed from directory names, that assumption was checked against
real includes, real link lines, and real Makefile rules.

---

## 1. Current Build Architecture

### 1.1 Orchestration

The build is a **three-layer recursive Make system** plus a nested external CMake build:

```text
                     make (root Makefile, 2503 lines, 320 targets)
                       │
       ┌───────────────┼────────────────┬──────────────────┬─────────────────┐
       │               │                │                  │                 │
  $(MAKE) -C apps   $(MAKE) $(FM_ELF)  $(MAKE) $(ST_ELF)  $(MAKE) $(TM_ELF)  $(MAKE) $(BW_ELF)
   (apps/Makefile)   (root rules)       (root rules)       (root rules)      (root rules)
       │               │                │                  │                 │
       └───────────────┴────────────────┴──────────────────┴─────────────────┘
                                       │
                       ┌───────────────┴────────────────┐
                       │                                │
        cmake -G "Unix Makefiles"              bash tools/*/run-qemu.sh
        (LLVM libc freestanding,               (QEMU smoke probes,
         build/libc/cmake/)                     serial-evidence grep)
```

There are **37 recursive `$(MAKE)` invocations**. Several are *not* for convenience but
are load-bearing for correctness (see §13.3 — the `| apps` order-only prerequisite on
`APP_ELFS` deliberately triggers a sub-make so that ELF files exist before the ISO rule
runs).

### 1.2 Project-owned build files

| File | Lines | Role |
| --- | --- | --- |
| `Makefile` | 2503 | Everything: kernel, lwIP, LLVM libc, C SDK, C++ SDK, libdesktop, C++ apps, Rust, ISO, QEMU, host tests, clean |
| `apps/Makefile` | 385 | User-space C/C++ apps: GUI apps, system utils, test ELFs, widget toolkit, font blobs |
| `limine/Makefile` | 19 | Upstream Limine host tool (builds `limine`/`limine.exe`) — **never invoked by the KyuzenOS build** |
| `third_party/freetype/kyuzen.mk` | 84 | FreeType freestanding module selection + port flags |
| `third_party/lexbor/kyuzen.mk` | 94 | Lexbor freestanding module selection + FP-ban guard |
| `limine.conf` | 277 | Limine boot config: 86 `module_path` entries |

`third_party/bearssl/Makefile`, `third_party/freetype/Makefile`, and the ~80 `.mk`
files under `third_party/freetype/builds/` are **upstream vendor build systems** and are
not part of the KyuzenOS build.

### 1.3 Repository scale

| Metric | Value |
| --- | --- |
| Tracked files (total) | 2750 |
| Tracked files (excluding `third_party/`) | 858 |
| Tracked files (`third_party/`) | 1892 |
| `third_party/stdlib/llvm-project` | **gitlink** (mode `160000`, commit `ca7933e4`) — no `.gitmodules`; ~1930 MB on disk |
| Kernel C sources | 111 |
| Kernel ASM sources | 12 |
| User-space app/library C++ sources | 37 (widget toolkit) + 47 (other) |
| Host test sources | 25 files in `tests/host/unit/` |

The gitlink without `.gitmodules` is a **pre-existing repository defect** (§13.4).

---

## 2. Existing Make Targets

320 targets total (86 `.PHONY` declarations). Grouped by function:

### 2.1 Build targets

| Target | Output | Notes |
| --- | --- | --- |
| `all` (default) | `build/bin/myos.bin` + `build/myos.bin` | Kernel only |
| `boot_image.iso` | `build/boot_image.iso` | Kernel + all ELFs + assets + Limine |
| `apps` | `build/apps/*.elf` | Umbrella: `sdk-c sdk-cpp libdesktop $(FT_KYUZEN_A)` then 6 sub-makes |
| `sdk-c` | `build/sdk/c/.staged` | C SDK stage (headers + `libc.a` + `crt.o` + `app.ld`) |
| `sdk-cpp` | `build/sdk/cpp/.staged` | C++ SDK stage (+ libc++ header closure + `libcxxrt.a` + wrapper) |
| `libdesktop` | `build/desktop/libdesktop.a` | Desktop framework archive |
| `desktop` | `build/apps/desktop.elf` | Implementation chosen by `DESKTOP_APP` |
| `filemanager` / `settings-app` / `taskmgr-app` / `browser-app` | respective ELFs | C++ SDK apps |
| `libc-phase0..7` | `build/libc/**` | LLVM libc bring-up phases |
| `lexbor-stage-a` | `build/libc/lexbor-stage-a/**` | Lexbor QEMU probe |
| `rust-apps` | `build/apps/{hello-slint,control-center}.elf` | Cargo workspace |
| `mkfs` | `./mkfs.kyuzenfs` | Host KyuzenFS formatter |
| `<name>.elf` (29 targets) | `build/apps/<name>.elf` | Convenience aliases |

### 2.2 Test targets (33 declared)

**Only 19 of 33 are buildable.** 13 are dead because their sources were deleted
(§13.1). The 14th (`test-kyuzenfs-v4`) is dead for the same reason.

| Buildable (19) | Dead (13) |
| --- | --- |
| `test-netsock`, `test-netdns`, `test-netutil` | `test-virtqueue`, `test-virtio-cmd` |
| `test-browser-url-http`, `test-browser-html`, `test-browser-css-layout` | `test-cred`, `test-proc`, `test-kill`, `test-fd`, `test-pipe`, `test-fork` |
| `test-tls`, `test-color`, `test-text`, `test-text-ft` | `test-kyuzenfs-v4`, `test-kyuzenfs-xcheck` |
| `test-lexbor-host`, `test-media`, `test-kyuzenfs-tree` | `test-panic`, `test-ata`, `test-textedit` |
| `test-libui-theme`, `test-libui-xml`, `test-libui-fileman`, `test-libui-owner` | |
| `test-desktop`, `test-libc-heap` | |

### 2.3 QEMU / probe targets

`run`, `run-serial`, `run-wd`, `stress`, `conc`, `heap-stress`, `heap-watch`,
`test-fileman-qemu`, `desktop-qemu`, `desktop-qemu-test`, `desktop-smoke`,
`cpp-app-run`, `sdk-c-smoke-qemu`, `libc-phase1-qemu`, `libc-phase2-qemu`,
`libc-phase4-qemu`, `sdk-cpp-smoke-qemu`, `libc-phase6-qemu`, `libc-phase7-qemu`,
`lexbor-stage-a-qemu`.

### 2.4 Utility targets

`compile_commands`, `libc-clean`, `rust-clean`, `clean-apps`, `clean-tool`, `clean`.

---

## 3. Toolchain

Resolved on this host (MSYS2 at `E:\Tools\msys2`):

| Tool | Version | Path |
| --- | --- | --- |
| `clang` / `clang++` | 21.1.8 | `usr\bin` (MSYS) and `clang64\bin` |
| `ld.lld` | 21.1.8 | `usr\bin` |
| `nasm` | 2.16.03 | `usr\bin` |
| `llvm-ar`, `llvm-nm`, `llvm-objdump` | LLVM 21.1.8 | `usr\bin` |
| `cmake` | 4.4.3 | `usr\bin` |
| `ninja` | 1.13.2 | `clang64\bin` |
| GNU `make` | 4.4.1 | `usr\bin` |
| `xorriso` | 1.5.6 | `usr\bin` |
| `python` | 3.14.6 | `clang64\bin` |
| `cargo` / `rustc` | 1.98.1 | `~\.cargo\bin` |
| `qemu-system-x86_64` | 10.2.0 | `qemu\` (not on PATH) |
| `bash` | 5.3.15 | `usr\bin` |

**Toolchain selection is implicit PATH resolution.** The Makefile assigns
`CC = clang`, `LD = ld.lld`, `AS = nasm` — no absolute paths, no version pinning.
This is portable but non-reproducible across host updates.

### 3.1 Target triple

`x86_64-pc-none-elf` — used consistently for:

- kernel (`CFLAGS`, §4.1)
- user-space apps (`apps/Makefile` `CFLAGS_COMMON`)
- LLVM libc cross-build (`LIBC_TRIPLE`)
- C SDK / C++ SDK app compilation
- FreeType freestanding (`FT_KYUZEN_CFLAGS`)
- Lexbor freestanding (`LEXBOR_KYUZEN_CFLAGS`)
- BearSSL OS build (`BL_OS_FLAGS`)

Host tests use the **host** triple (no `--target` flag).

### 3.2 Kernel objects are `--target=x86_64-pc-none-elf`, not `-m64`

The kernel compiles with `--target=x86_64-pc-none-elf`, which selects the
bare-metal ABI. `-mcmodel=kernel` is added on top. This matters for CMake:
`CMAKE_C_COMPILER_TARGET` must carry the triple so that
`-mcmodel=kernel` + higher-half relocation behaviour is preserved.

### 3.3 `-mno-sse` is load-bearing across the whole OS

The kernel never sets `CR4.OSFXSR`, so **any SSE instruction faults**. Every
user-space component inherits `-mno-sse -mno-sse2 -mno-mmx -msoft-float`:

- kernel `CFLAGS`
- `apps/Makefile` `CFLAGS_COMMON`
- LLVM libc `LIBC_TARGET_FLAGS`
- FreeType, Lexbor, BearSSL

Consequently, source files that return `double` fail to compile
("SSE register return with SSE disabled"). This is why Lexbor's
`core/{conv,dtoa,strtod,diyfp}.c` are excluded and why libc++'s `string.cpp` /
`algorithm.cpp` are replaced by Kyuzen-authored "slice" TUs. **The migration must
preserve these exclusions exactly** — they are correctness constraints, not
optimization choices.

---

## 4. Compiler Flags

### 4.1 Kernel

```text
CFLAGS = --target=x86_64-pc-none-elf -ffreestanding -O2 -nostdlib \
         -mcmodel=kernel -mno-red-zone -mno-sse -mno-sse2 -mno-mmx \
         -msoft-float -MMD -MP \
         -Iinclude -Igraphics -Igraphics/memory -Idrivers/graphics/hw \
         -Ilibs/gui/color/include \
         -DATA_READ_PATH_DEFAULT=$(ATA_READ_PATH_DEFAULT)
```

plus `-std=c11` appended in the recipe.

### 4.2 lwIP (same as kernel, plus)

```text
LWIP_CFLAGS = $(CFLAGS) -std=c11 -Ithird_party/net/lwip/src/include \
              -Idrivers/net/port -Wno-error
```

Four kernel files need `LWIP_CFLAGS` and have **per-file overrides**:
`kernel/net/net_init.c`, `net_ping.c`, `net_socket.c`, `net_dns.c`. In CMake these
become four `set_source_files_properties(... COMPILE_OPTIONS ...)` calls, or a small
separate target for the lwIP-dependent kernel sources.

### 4.3 User-space C (apps/Makefile)

```text
CFLAGS_COMMON = --target=x86_64-pc-none-elf -ffreestanding -nostdlib \
                -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -msoft-float \
                -MMD -MP -Iinclude -Ilibs/gui/color/include
CFLAGS_APP    = $(CFLAGS_COMMON) -O0      # apps/
CFLAGS_LIB    = $(CFLAGS_COMMON) -O2      # libraries, system utils
```

Note: user-space does **not** get `-mcmodel=kernel` (apps live at `0x4000000`).

### 4.4 User-space C++ (widget toolkit)

```text
CXXFLAGS_LIB = $(CFLAGS_COMMON) -O2 -fno-exceptions -fno-rtti \
               -fno-use-cxa-atexit -fno-threadsafe-statics -std=c++17
```

The freestanding C++ contract is explicit: no exceptions, no RTTI, no
`__cxa_atexit`, no thread-safe statics. `operator new`/`delete` and
`__cxa_pure_virtual` are stubbed **once** in
`libs/gui/widget/src/runtime/runtime.cpp` (ODR — must not be redefined elsewhere).

The `app.ld` for this group has `ENTRY(main)` and **does not run `.init_array`**, so
global constructors must not be used.

### 4.5 SDK-based apps (C and C++)

```text
LIBC_TARGET_FLAGS = --target=x86_64-pc-none-elf -ffreestanding -nostdlib \
                    -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -msoft-float
SDK_CFLAGS    = $(LIBC_TARGET_FLAGS) -O2
SDK_CXXFLAGS  = $(LIBC_TARGET_FLAGS) -O2 -std=c++17 -fno-exceptions -fno-rtti -nostdinc++
```

`SDK_CXXFLAGS` includes `-nostdinc++` to stay hermetic from the host libc++.

### 4.6 LLVM libc port layer

```text
LIBC_PORT_CFLAGS = $(LIBC_TARGET_FLAGS) -std=gnu++17 -fno-exceptions -fno-rtti -O2 \
                   -fno-builtin -fno-unwind-tables -fno-asynchronous-unwind-tables \
                   -fvisibility-inlines-hidden \
                   -Ithird_party/stdlib/llvm-project/libc \
                   -Ibuild/libc/cmake/libc -isystem build/libc/cmake/libc/include \
                   $(LIBC_PORT_DEFS)
```

with

```text
LIBC_PORT_DEFS = -DLIBC_NAMESPACE=__llvm_libc_22_1_8_ -DLIBC_FULL_BUILD \
                 -DLIBC_TARGET_OS_IS_BAREMETAL \
                 -DLIBC_ERRNO_MODE=LIBC_ERRNO_MODE_EXTERNAL \
                 -DLIBC_THREAD_MODE=LIBC_THREAD_MODE_SINGLE \
                 -DLIBC_COPT_PUBLIC_PACKAGING
```

The namespace define **must match** the libc version string exactly.

### 4.7 FreeType (freestanding)

```text
FT_KYUZEN_CFLAGS = --target=x86_64-pc-none-elf -ffreestanding -nostdlib \
                   -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -msoft-float -O2 -std=c11 \
                   -MMD -MP \
                   -DFT2_BUILD_LIBRARY \
                   -D'FT_CONFIG_STANDARD_LIBRARY_H=<ftkz_stdlib.h>' \
                   -D'FT_CONFIG_MODULES_H=<ftkz_modules.h>' \
                   -DKZFONT_USE_FREETYPE \
                   -Ithird_party/freetype/kyuzen/include -Ithird_party/freetype/include \
                   -Ilibs/text/include -Ilibs/gui/color/include
```

The `<...>` single-quoting is a shell-quoting workaround for passing a
`-DNAME=<header>` macro through `sh`. **CMake passes arguments directly with no
shell**, so the quotes must be dropped (use `-DFT_CONFIG_STANDARD_LIBRARY_H=<ftkz_stdlib.h>`
as a single list element).

### 4.8 Lexbor (freestanding)

```text
LEXBOR_KYUZEN_CFLAGS = $(LIBC_TARGET_FLAGS) -O2 -std=c11 -DLEXBOR_STATIC -MMD -MP \
                       -Ithird_party/lexbor/kyuzen/include \
                       -Ithird_party/lexbor/source -isystem $(SDK_INC)
```

Include **order matters**: the Kyuzen `memory.h` shim must win over the host header.

### 4.9 BearSSL OS build

```text
BL_OS_FLAGS = $(LIBC_TARGET_FLAGS) -O2 -std=c11 \
              -DBR_USE_UNIX_TIME=0 -DBR_USE_WIN32_TIME=0 \
              -DBR_USE_URANDOM=0 -DBR_USE_WIN32_RAND=0 -DBR_RDRAND=0 \
              -DBR_AES_X86NI=0 -DBR_SSE2=0 \
              -Ithird_party/bearssl/inc -Ithird_party/bearssl/src \
              -isystem $(SDK_INC) -iquote include -Iapps/browser/tls
```

### 4.10 Host builds

```text
HOSTCC  = clang
HOSTCXX = clang++
```

No `--target` flag. Flags vary per test: `-O1` or `-O2`, `-Wall -Wextra`, and a mix of
`-I` / `-iquote` (the distinction matters — see §13.2).

---

## 5. Linker Configuration

### 5.1 Kernel

```text
LDFLAGS = -flavor gnu -T linker.ld -m elf_x86_64 --build-id=none -nostdlib
```

`linker.ld`: higher-half, `ENTRY(kernel_main)`, `KERNEL_VMA = 0xFFFFFFFF80000000`,
sections `.text .rodata .requests .data .bss`, `/DISCARD/` on `.note*`, `.eh_frame`,
`.multiboot`.

`.requests` uses `KEEP()` on the Limine request markers — required for the protocol.

### 5.2 User-space ELFs

Three linker scripts, all linking at `0x4000000`:

| Script | `ENTRY` | Used by |
| --- | --- | --- |
| `apps/app.ld` | `main` | All `apps/Makefile`-built ELFs, Rust ELFs |
| `libs/c/linker/app.ld` | `_start` | C SDK apps (clock, smoke apps) |
| `libs/cpp/linker/app.ld` | `_start` | C++ SDK apps (desktop, fileman, settings, taskmgr, browser) |

All three declare exactly two `PT_LOAD` segments (`text` R-X, `data` RW-) and discard
`.comment .note* .gnu* .eh_frame* .debug*`. The kernel ELF loader only understands
`PT_LOAD`, so any extra segment is a crash.

The C++ script additionally arranges `.init_array` so the CRT can walk it — which is
why `SDK_CXXFLAGS` apps *can* use global constructors while `CXXFLAGS_LIB` apps cannot.

### 5.3 Link line shapes

| Consumer | Link command |
| --- | --- |
| Kernel | `ld.lld $(LDFLAGS) $(ALL_OBJS) -o $@` |
| GUI app | `ld.lld -m elf_x86_64 -nostdlib -T app.ld $< $(GUI_LIBS) -o $@` |
| System util | `ld.lld -m elf_x86_64 -nostdlib -T app.ld $< $(USERLIB_OBJ) -o $@` |
| C SDK app | `ld.lld -m elf_x86_64 -nostdlib -T $(SDK_LD) $< $(SDK_CRT) $(SDK_LIB) -o $@` |
| C++ SDK app | wrapper `kyuzen-c++` → `ld.lld ... objects libcxxrt.a cxxrt.o crt.o libc.a` |

**Library order is load-bearing** (static archives are resolved left-to-right):
`libc.a` must be last for C apps; `libcxxrt.a → cxxrt.o → crt.o → libc.a` for C++ apps.
CMake's `target_link_libraries` preserves the declared order, so this is expressible —
but it must be declared deliberately, not alphabetically.

---

## 6. Application Discovery

### 6.1 Kernel source discovery

```make
SRC_DIRS = arch/x86 drivers kernel kernel/syscall kernel/smp kernel/gfx kernel/sched \
           kernel/fs kernel/net kernel/mm kernel/debug kernel/sync kernel/proc \
           libs/core system kernel/panic graphics graphics/backend \
           graphics/backend/intel graphics/memory drivers/graphics/hw libs/gui/color/src

C_SOURCES_RAW = $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.c))
ASM_SOURCES   = $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.asm))
```

Then a **long explicit `filter-out`** removes 29 files that belong to other targets:
user-space libs (`userlib.c`, `libgui.c`, `netutil.c`, `userutil.c`, `media.c`),
`system/*.c` (cat/echo/zen/init/login/shell/shell_core), and host tests that happen to
sit in a `SRC_DIRS` directory.

Resolved: **111 C sources + 12 ASM sources.**

> The comment in the Makefile warns that a recursive `find` from root would
> accidentally pick up `third_party/*`, `legacy/`, `tests/`, `debug/`, `rust/target`.
> Rule 18 (no `file(GLOB_RECURSE)`) aligns with this: the directory lists are
> deliberate.

`SRC_DIRS` is also **overridable** for stress builds:
`make boot_image.iso SRC_DIRS="$(SRC_DIRS) debug" CFLAGS="$(CFLAGS) -DSTRESS_TEST"`.
(`debug/` does not currently exist — the `stress` target is dead.)

### 6.2 Application discovery

`apps/Makefile` groups apps by **build recipe**, not by directory:

| Group | Apps | Link inputs |
| --- | --- | --- |
| `GUI_APPS` | viewer clock calc notepad widget_demo fontdemo xml_demo | `GUI_LIBS` |
| `SYS_APPS` | echo cat zen init login shell | `USERLIB_OBJ` (shell/login extra) |
| `TEST_APPS` | procinfo exit_test kill_test fd_test pipe_test fork_test badptr | `USERLIB_OBJ` |
| `OTHER_APPS` | terminal | `TERMINAL_LIBS` |
| `CPP_APPS` | gallery imageview | `MEDIA_LIBS` |

Built by the **root** Makefile (not `apps/Makefile`), each with its own rule:
`desktop.elf` (from `system/$DESKTOP_APP/`), `fileman.elf` (`apps/filemanager/`),
`settings.elf` (`apps/settings/`), `taskmgr.elf` (`apps/taskmgr/`),
`browser.elf` (`apps/browser/`).

So there are **three different application build recipes** in the project, and
`apps/Makefile`'s `ALL_NAMES` deliberately excludes the root-built ones.

### 6.3 Why `apps/Makefile` and the root Makefile must stay in sync

`APP_NAMES` in the root Makefile (line 1941) must match `apps/Makefile`'s
`ALL_NAMES` plus the root-built apps. This duplicated list is a **maintenance hazard**
and one of the strongest arguments for CMake: one `add_executable` per app removes the
possibility of the two lists diverging.

---

## 7. Library Dependency Graph

Verified from actual `-I` flags, `-D` defines, and link lines — **not** from directory
names. Several libraries are compiled *twice* with different flags (kernel vs
user-space), which is a real dependency-graph subtlety.

```text
libs/gui/color ────────────────────────────────┬──► KERNEL (CFLAGS, -mcmodel=kernel)
  (color_blend, color_space, color_utils)      │
                                               └──► USER-SPACE (CFLAGS_LIB)
                                                        └──► GUI_APPS, terminal,
                                                             gallery, imageview,
                                                             fileman, settings,
                                                             taskmgr, browser

libs/core/userlib.c ──► every app ELF (syscall wrappers; NOT in kernel)
libs/core/userutil.c ─► shell.elf, login.elf, terminal.elf
libs/core/netutil.c ──► shell.elf, terminal.elf, desktop.elf
libs/core/libgui.c ───► GUI_APPS, terminal, desktop.elf (font8x16)
libs/core/libgui.c ──► NOT in kernel (excluded)

libs/media/png.c ─────► GUI_APPS, gallery, imageview, terminal
libs/media/media.c ───► gallery, imageview, fileman (NOT in kernel)

libs/gui/widget/**/*.cpp (37 sources) ──► GUI_APPS, terminal, gallery, imageview,
  + libs/gui/widget/abi/libui_abi.cpp     fileman, settings, taskmgr, browser
  (NOT in kernel)

libs/text/src/kzfont.c ──► fontdemo, settings, desktop, browser
libs/text/src/kzraster_ft.c ──► same + FreeType archive

libs/gui/libdesktop/src/*.cpp (5) ──► desktop.elf, dtsmoke, test-desktop

third_party/freetype (16 TU) ──► build/lib/libfreetype_kyuzen.a
    └──► fontdemo, settings, desktop, browser

third_party/lexbor (module closure) ──► build/lib/liblexbor_kyuzen.a
    └──► browser

third_party/bearssl (subset, 60+ TU) ──► browser.elf objects only
    (NOT a separate archive — objects linked directly)
```

### 7.1 Kernel vs user-space duplication (critical)

| Source | Kernel | User-space | Why |
| --- | --- | --- | --- |
| `libs/gui/color/src/*.c` | ✔ | ✔ | Pure integer colour math needed by both compositor and widgets |
| `libs/text/src/kzfont.c` | ✘ | ✔ | Font rendering is user-space only |
| `libs/media/media.c` | ✘ | ✔ | All syscalls — would be dead code in kernel |
| `libs/core/userlib.c` | ✘ | ✔ | Defines `int $0x80` wrappers |

**They cannot share object files**: different flags (`-mcmodel=kernel`, different
`-O`), different ABI assumptions. The Makefile uses two object namespaces
(`build/obj/...` for kernel, `build/obj/user/...` for apps) precisely for this.

In CMake this is naturally handled by two separate targets
(`kyuzen-color-kernel` INTERFACE/target vs `kyuzen-color`), which is cleaner than
namespace-separated object directories.

### 7.2 Kernel subsystem breakdown

| Subsystem | Sources | Notes |
| --- | --- | --- |
| `arch/x86` | 3 C + 12 ASM | GDT, IDT, LAPIC, ISR stubs |
| `drivers` | 9 C | acpi, ata, keyboard, mouse, pci, rtc, serial, timer, tty |
| `kernel/syscall` | 10 C | one file per syscall family |
| `kernel/sched` | 5 C | block, core, debug, lifecycle, runqueue |
| `kernel/fs` | 10 C | KyuzenFS V4 (bcache, kfs_*, vfs) |
| `kernel/net` | 4 C | + 76 lwIP sources + 3 e1000/port sources |
| `kernel/mm` | 5 C | heap, heap_watch, paging, pmm, uheap |
| `kernel/sync` | 4 C | event, spinlock, sync, wait |
| `kernel/proc` | 3 C | elf, proc, usercopy |
| `kernel/panic` | 4 C | panic, panic_draw, panic_explain, panic_hw |
| `kernel/debug` | 3 C | crashdump, crash_archive, panic_log |
| `kernel/gfx` | 3 C | compositor, fb, kwm |
| `graphics` | 3 + 27 Intel backend | ghal, software, virtio_gpu, intel_gen12_* |
| `drivers/graphics/hw` | 3 C | virtio_gpu_cmd, virtio_gpu_dev, virtqueue |
| `kernel/*` (top) | 8 C | boot_console, cpu, display, entropy, kernel, kprint, string, timer_callbacks |
| lwIP | 76 C | 3 explicit groups, 2 flag sets |
| e1000 | 1 C | kernel CFLAGS (standalone) |

---

## 8. Third-Party Dependencies

| Dependency | Location | Classification | Integration |
| --- | --- | --- | --- |
| **LLVM libc** | `third_party/stdlib/llvm-project/libc` | **A + G** (own CMake; needs freestanding config) | External CMake build, `-G "Unix Makefiles"`, driven by cache file `x86_64-pc-none-elf.cmake` + `config/baremetal/x86_64/{entrypoints,headers}.txt`. Output copied to `build/libc/`. |
| **libc++ (subset)** | `…/libcxx` | **D** (vendored sources compiled directly) | Only `src/stdexcept.cpp` + `src/verbose_abort.cpp` compiled; `string.cpp`/`algorithm.cpp` replaced by Kyuzen slices. Header *closure* computed at build time via `clang -M`. |
| **lwIP** | `third_party/net/lwip` | **D + G** | Sources compiled by the kernel build with `LWIP_CFLAGS`. Explicit netif list (ethernet.c only); slipif/zepif/bridgeif/lowpan6/ppp excluded. |
| **FreeType** | `third_party/freetype` | **A (has CMake) but not used** | Custom `kyuzen.mk` selects 14 upstream TUs + 2 Kyuzen port TUs. Upstream `CMakeLists.txt` exists but is **not** used for the freestanding target. |
| **Lexbor** | `third_party/lexbor` | **D + G** | Custom `kyuzen.mk`; module closure (core/dom/html/ns/tag). 4 FP sources banned. |
| **BearSSL** | `third_party/bearssl` | **D + E + F** | **Two builds**: (a) full host library → `build/host-tls/brssl` tool; (b) minimal OS subset compiled with `BL_OS_FLAGS` and linked into `browser.elf`. |
| **stb_image** | `include/stb_image.h` | **C** (header-only) | Included directly by `apps/browser/imgdec.c`. |
| **Limine** | `limine/` | **B** (own Makefile, never invoked) | Prebuilt binaries committed/ignored. `limine.exe` is a **host tool that must pre-exist** (gitignored, not built by the KyuzenOS build). |

### 8.1 LLVM libc is a *nested build system*

This is the single most complex integration. The root Makefile:

1. Configures LLVM's `runtimes` superbuild with `cmake -C <cache> -G "Unix Makefiles"`
   into `build/libc/cmake`.
2. Builds target `libc` with `-- -j$(LIBC_JOBS)`.
3. Copies `build/libc/cmake/libc/lib/libc.a` → `build/libc/x86_64-pc-none-elf/lib/libc.a`.
4. Compiles the Kyuzen port layer (`kyuzen_libc_port.cpp`) against libc's internal headers.
5. Stages a public SDK (`build/sdk/c`) containing only public headers + archive + `crt.o` + `app.ld`.

**CMake migration note:** the outer CMake project can drive this with
`ExternalProject_Add` or `add_custom_command`, but must not try to make it a
subdirectory (different generator, different toolchain, different binary dir).
Preserving `-G "Unix Makefiles"` for the nested build is acceptable — Rule 7 applies
to the *primary* generated backend.

### 8.2 The C++ SDK staging step computes a header closure

`$(SDK_CPP_STAGE)` runs, for each of 8 public libc++ headers:

```sh
echo "#include <$h>" | clang++ $(LIBCXX_BUILD_FLAGS) -x c++ -M -MT x - \
  | tr ' ' '\n' | grep "^$(LIBCXX_INCLUDE)/" | sed 's|\\$||'
```

then copies the resulting closure into `build/sdk/cpp/include/`. This is a
**build-time computed dependency set**, not a static list. In CMake it becomes a
custom command with `DEPFILE` or an explicit `add_custom_command` OUTPUT set.

---

## 9. Image Generation Pipeline

### 9.1 Exact dependency order

```text
build/boot_image.iso
  ├── build/bin/myos.bin          (kernel; depends on 123 objects)
  ├── build/myos.bin              (compat copy; cmp-guarded so it never re-touches)
  ├── build/apps/*.elf × 30       (APP_ELFS 29 + RUST_ELFS 2, minus overlap)
  ├── limine.conf                 (or LIMINE_CONF override)
  ├── assets/logo/{kyuzen,logo-splash}.png
  ├── manifests/*.app × 30
  ├── limine/{BOOTX64.EFI, limine-bios.sys, limine-bios-cd.bin, limine-uefi-cd.bin}
  ├── DESKTOP_ASSETS (12 icons + 6 wallpapers)
  ├── FONT_ASSETS (5 TTF)
  └── SHELL_ASSETS (assets/shell/neofect.json)
```

### 9.2 Recipe steps

1. `mkdir -p build/iso_root/EFI/BOOT`
2. `rm -f build/iso_root/*.elf` — **stale-ELF sweep** so deleted apps don't linger
3. Copy ELFs + kernel + `limine.conf` + logo + manifests + Limine files to `iso_root/`
4. Copy `logo-splash.png` → `iso_root/logo.png` (rename)
5. Copy desktop/font/shell assets **flat** (basename only) to `iso_root/`
6. Conditionally copy optional smoke-test apps if they exist (10 checks)
7. Copy `BOOTX64.EFI` → `iso_root/EFI/BOOT/`
8. **Module guard**: parse every `boot():/PATH` from `$(LIMINE_CONF)`, verify each exists
   in `iso_root/`, fail the build listing the missing files
9. `xorriso -as mkisofs -b limine-bios-cd.bin -no-emul-boot -boot-load-size 4 \
   -boot-info-table --efi-boot limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
   --protective-msdos-label iso_root -o boot_image.iso`
10. `./limine/limine.exe bios-install boot_image.iso`

The **module guard (step 8)** is a genuine correctness feature that must be preserved:
Limine panics at boot with "Failed to open module with path" if a listed module is
missing, and that failure is only visible at runtime. The guard converts it into a
build error. It exists because `neofect.json` was listed in `limine.conf` but never
copied — a real bug that shipped.

The staging directory is **synced, not `rm -rf`'d** — this keeps the ISO target
incremental (only rebuilds when inputs change).

### 9.3 ISO scale

- 86 `module_path` entries in `limine.conf`
- 92 files in `iso_root/`
- Output: **31,617,024 bytes** (30.2 MB)

---

## 10. Test Pipeline

### 10.1 Structure

```text
tests/
  host/
    unit/      25 files  — 19 buildable host test binaries
    probes/    14 Python files — GUI/QEMU probes (not Make targets)
  target/       7 C files — ELFs that run inside QEMU
  browser_site/ Python TLS test server + certs
```

### 10.2 Host test build patterns

| Pattern | Example | Flags |
| --- | --- | --- |
| Pure policy, header-only | `cred_test` | `-O2 -Wall -Wextra -Iinclude` |
| Real source + mocks | `netsock_test` | `-O1 -DKSOCK_HOST_TEST -iquote netstub -iquote include` |
| Real source compiled as C | `media_host.o` | `-iquote include` (so host `<stdlib.h>` wins) |
| Full toolkit linked | `libui_*_test` | `-std=c++17 -iquote include -Ilibs/gui/widget/include` |
| C++ engine linked | `browser_html_test` | `-std=c++17 -DLEXBOR_STATIC` + host Lexbor archive |
| Freestanding smoke check | `text_test` | Also compiles `kzfont.c` with kernel flags as a "no host libc" proof |

The `-iquote` vs `-I` distinction is deliberate: `include/` contains a
`stdlib.h` shim (`stb_image`), and using `-Iinclude` would shadow the host's
`<stdlib.h>`, breaking libc++ headers. **CMake's `target_include_directories` needs
careful `SYSTEM`/`PRIVATE`/`INTERFACE` choices to reproduce this.**

### 10.3 Baseline results (before migration)

```text
17 PASS / 2 FAIL  (of 19 buildable)
```

| Test | Result | Cause |
| --- | --- | --- |
| 17 tests | **PASS** | — |
| `test-kyuzenfs-tree` | **FAIL** | Binary builds, but exits `0xC0000016` (STATUS_ILLEGAL_INSTRUCTION) with no output. Pre-existing; not build-related. |
| `test-libc-heap` | **FAIL** | Compile error: `no member named 'write_to_stderr' in namespace '__llvm_libc_22_1_8_'` — LLVM libc internal API drift. Pre-existing. |

13 additional `test-*` targets cannot be built at all (missing sources, §13.1).

**These 2 failures and 13 dead targets are the migration baseline.** The migration must
not make this worse; it is not expected to fix them (they are source-level issues, not
build-system issues).

---

## 11. QEMU Pipeline

### 11.1 Primary targets

| Target | CPUs | RAM | Display | Serial |
| --- | --- | --- | --- | --- |
| `run` | 8 | 1G | `gtk,zoom-to-fit=on` + `-full-screen` | `file:serial.log` |
| `run-serial` | 4 | 1G | GTK | `stdio` |
| `run-wd` | 4 | 1G | GTK | `file:serial.log` |
| `stress` / `conc` / `heap-stress` | 4 | 512M | GTK | — |
| `heap-watch` | 4 | 512M | GTK | `stdio` |

Common hardware configuration:

```text
-cpu max
-boot d
-drive file=disk.img,format=raw,index=0,media=disk
-drive file=build/boot_image.iso,media=cdrom,index=2
-nic user,model=e1000
-vga none -device virtio-vga,xres=1920,yres=1080
```

`QEMU_DISPLAY` and `FULLSCREEN` are overridable:
`make run FULLSCREEN=0 QEMU_DISPLAY=none`.

### 11.2 Probe scripts

`tools/*/run-qemu.sh` drive automated QEMU smoke tests, configured entirely by
environment variables:

```text
KYUZEN_TEST_APP_PATH, KYUZEN_TEST_APP, KYUZEN_TEST_START, KYUZEN_TEST_DISK,
KYUZEN_TEST_SERIAL, KYUZEN_TEST_MONLOG, KYUZEN_TEST_MARKER,
KYUZEN_TEST_SUCCESS, KYUZEN_TEST_FAILURE, QEMU
```

They build a throwaway disk image, boot the ISO, inject keystrokes via the QEMU
monitor, and grep the serial log for a success marker. `QEMU` is passed explicitly
because `qemu-system-x86_64` is **not on PATH** in this environment.

### 11.3 Environment assumption

The Makefile's `QEMU = qemu-system-x86_64.exe` is a bare name; the probe scripts
receive `QEMU="$(QEMU)"`. Both rely on PATH or explicit override. **CMake must expose
this as a cache variable** (`KYUZEN_QEMU`) defaulting to `qemu-system-x86_64`.

---

## 12. Generated Artifacts

### 12.1 Full inventory

| Class | Path | Count | Tracked? |
| --- | --- | --- | --- |
| **KERNEL** | `build/bin/myos.bin` | 1 | ✘ (`build/`) |
| **KERNEL (compat)** | `build/myos.bin` | 1 | ✘ |
| **USER APPLICATION** | `build/apps/*.elf` | 30 | ✘ (`*.elf`) |
| **STATIC LIBRARY** | `build/lib/libfreetype_kyuzen.a` | 1 | ✘ |
| **STATIC LIBRARY** | `build/lib/liblexbor_kyuzen.a` | 1 | ✘ |
| **STATIC LIBRARY** | `build/desktop/libdesktop.a` | 1 | ✘ |
| **STATIC LIBRARY** | `build/sdk/c/lib/libc.a` | 1 | ✘ |
| **STATIC LIBRARY** | `build/sdk/cpp/lib/libcxxrt.a` | 1 | ✘ |
| **OBJECT** | `build/obj/**/*.o` | 293 | ✘ (`*.o`) |
| **DEPFILE** | `build/obj/**/*.d` | 402 | ✘ (`*.d`) |
| **BOOT ARTIFACT** | `build/boot_image.iso` | 1 | ✘ (`*.iso`) |
| **STAGING** | `build/iso_root/` | 92 | ✘ (`build/`) |
| **SDK STAGE** | `build/sdk/{c,cpp}/` | — | ✘ |
| **HOST TEST** | `tests/host/unit/<name>` (+ `.exe`) | 19 | ✘ (`*.exe`) |
| **HOST TEST OBJ** | `tests/host/unit/media_host.o`, `color_utils_host.o` | 2 | ✘ (`*.o`) |
| **HOST TOOL** | `mkfs.kyuzenfs` | 1 | ✘ (explicit) |
| **HOST TOOL** | `build/host-tls/brssl` | 1 | ✘ |
| **RUST ARTIFACT** | `rust/target/**` | — | ✘ (`rust/.gitignore`) |
| **DISK IMAGE** | `disk.img`, `test_disk.img` | 2 | ✘ (`*.img`) |
| **GENERATED SOURCE** | `apps/browser/tls/tas_{testca,https}.inc` | 2 | ✘ |
| **GENERATED CONFIG** | `build/libc/iso*/limine.conf` | 7 | ✘ |
| **RUNTIME** | `serial.log` | 1 | ✘ (explicit) |
| **IntelliSense** | `compile_commands.json` | 1 | ✘ (explicit) |

### 12.2 Untracked-but-present in the working tree

`disk.img` (100 MB), `test_disk.img` (100 MB), `serial.log`, `compile_commands.json`,
`fix/` — all gitignored. **None may become CMake source inputs.**

### 12.3 `build/` is entirely disposable

`make clean` runs `rm -rf build`, which destroys the LLVM libc CMake tree too. A
subsequent `make boot_image.iso` must rebuild libc from scratch (~7 minutes of the
416 s full build). **CMake must keep this property**: `rm -rf build && cmake -S . -B build`
must work, which means the nested LLVM libc build has to live under `build/` as well.

---

## 13. Current Build-System Problems

### 13.1 13 test targets are dead (missing sources) — HIGH

The Makefile declares build rules for sources that do not exist:

```text
tests/host/unit/{virtqueue_test,virtio_gpu_cmd_test,cred_test,proc_test,
                 kill_test,fd_test,pipe_test,fork_test,kyuzenfs_v4_test,
                 kyuzenfs_xcheck,panic_test,ata_devmodel_test,textedit_test}.{c,cpp}
```

`make test-cred` → `make: *** No rule to make target 'tests/host/unit/cred_test.c'`.

These files are also referenced in `C_SOURCES`'s `filter-out` list, so the exclusion
list documents files that no longer exist. **Pre-existing; not caused by the
migration.** The CMake build does not silently drop these targets: it registers them
behind an `if(EXISTS <source>)` guard with an `else()` branch that emits a
configure-time warning, so the breakage stays visible. See
`tests/host/CMakeLists.txt` and `cmake-migration.md`.

### 13.2 `-iquote` vs `-I` is load-bearing but implicit — HIGH

`include/stdlib.h` (an `stb_image` shim) shadows the host `<stdlib.h>` when `-Iinclude`
is used. Several host tests must use `-iquote include` instead. This is documented only
in scattered comments. CMake's include-directory model (`PRIVATE`/`INTERFACE`/`SYSTEM`)
must reproduce this precisely or libc++ headers break.

### 13.3 The ISO rule depends on a phony-triggered sub-make — HIGH

```make
$(filter-out ...,$(APP_ELFS)): | apps
```

The `| apps` order-only prerequisite exists so that `make boot_image.iso` triggers
`$(MAKE) -C apps all` (and the root-built apps) before the ISO rule reads the ELF
files. Four ELFs are deliberately excluded to avoid a fork-bomb:

```make
$(filter-out $(DESKTOP_ELF) $(ELF_DIR)/fileman.elf $(ELF_DIR)/settings.elf \
             $(ELF_DIR)/taskmgr.elf $(ELF_DIR)/browser.elf,$(APP_ELFS)): | apps
```

The comment warns that referencing `$(FM_ELF)` there (defined later in the file) would
expand to an empty string and re-introduce the fork-bomb. This is exactly the kind of
fragile ordering that explicit CMake target dependencies eliminate.

### 13.4 `third_party/stdlib/llvm-project` is a gitlink without `.gitmodules` — HIGH

```text
$ git ls-files -s third_party/stdlib
160000 ca7933e47d3a3451d81e72ac174dcb5aa28b59d1 0  third_party/stdlib/llvm-project
$ git submodule status
fatal: no submodule mapping found in .gitmodules for path 'third_party/stdlib/llvm-project'
```

A fresh clone cannot materialise this directory. `make sdk-c` / `make apps` /
`make boot_image.iso` all fail without it. **Pre-existing.**

### 13.5 `limine.exe` and the `*.bin` Limine files are gitignored but required — MEDIUM

`limine/.gitignore` ignores `limine` and `limine.exe`; the root `.gitignore` ignores
`*.bin`, which swallows `limine-bios-cd.bin` and `limine-uefi-cd.bin`. The ISO rule
requires all four files. `limine/Makefile` is **never invoked** by the build, so
`limine.exe` must be produced out-of-band (`make -C limine`). A fresh clone cannot
build an ISO.

### 13.6 `make clean` destroys the nested CMake build — MEDIUM

`rm -rf build` removes `build/libc/cmake`, forcing a full LLVM libc rebuild (~7 min).
There is no way to clean only the kernel/apps. The Makefile even documents the
workaround ("hapus build/libc/cmake lalu ulangi dari make libc-phase0") in an error
message.

### 13.7 Stale-artifact guards are manual — MEDIUM

Three separate stamp files exist to work around Make's timestamp-only model:

- `build/ata_read_path_default.stamp` — because `ATA_READ_PATH_DEFAULT` is a variable
  and "make only sees timestamps, not variable contents"
- `build/desktop/.selected` — forces relink when `DESKTOP_APP` changes
- `build/libc/**` guards in `$(SDK_STAGE)` — `llvm-nm` greps to detect stale archives

The `llvm-nm` guards exist because incremental CMake inside LLVM libc "does not detect
entrypoints.txt changes". CMake's `configure_file`/`DEPENDS` model handles the first
two natively; the third remains a genuine cross-build-system limitation.

### 13.8 37 recursive `$(MAKE)` calls — MEDIUM

Each is a separate process that re-parses a 136 KB Makefile. On Windows this costs
roughly 20 ms per spawn (the Makefile itself notes this for `mkdir`). `make apps`
issues 6 sub-makes serially.

### 13.9 Duplicated source lists — MEDIUM

`APP_NAMES` (root, line 1941) vs `ALL_NAMES` (apps/Makefile, line 168) must agree.
The root list also includes root-built apps that `apps/Makefile` excludes, so neither
list is derivable from the other.

### 13.10 `compile_commands.json` generation needs a third-party tool — LOW

```make
compile_commands:
	env -u MAKELEVEL python -m compiledb -n make clean all
```

Requires `pip install compiledb`, runs a *destructive* `make clean`, and needs a
`env -u MAKELEVEL` workaround. CMake emits `compile_commands.json` natively with
`CMAKE_EXPORT_COMPILE_COMMANDS=ON`.

### 13.11 `.vscode/c_cpp_properties.json` hardcodes a developer path — LOW

```json
"compilerPath": "E:/Tools/msys2/clang64/bin/clang.exe"
```

Rule: no hardcoded developer-specific absolute paths. This should become a relative /
discovered path once `compile_commands.json` is CMake-generated.

### 13.12 `stress`, `conc`, `heap-stress` reference a non-existent `debug/` — LOW

```make
stress:
	$(MAKE) boot_image.iso SRC_DIRS="$(SRC_DIRS) debug" CFLAGS="$(CFLAGS) -DSTRESS_TEST"
```

`debug/` does not exist, so this target is dead. `conc`/`heap-stress` point at
`tests/host/unit` (which exists) but rely on `-DCONC_TEST` / `-DHEAP_STRESS_TEST`
being honoured by sources that may not check them.

### 13.13 No `install` target, no `--target help` — LOW

Discovery of the 320 targets requires reading the Makefile.

---

## Appendix A — Verified baseline measurements

```text
Kernel sources (C)              111
Kernel sources (ASM)             12
Kernel objects                  123
Kernel build time (clean)       2.7 s
Kernel warnings                   0
Kernel binary              434,352 B   sha256 96b79a92…e827cb
Full build (clean → ISO)        416 s
ISO                       31,617,024 B
App ELFs                         30
Object files                    293
Depfiles                        402
ISO files                        92
limine.conf module_path entries  86
Makefile lines                 2503
Makefile targets                320
Makefile .PHONY declarations     86
Recursive $(MAKE) calls          37
Declared test targets            33
Buildable test targets           19   (17 pass, 2 fail)
Dead test targets                13
Tracked files (total)          2750
Tracked files (excl third_party) 858
```

## Appendix B — Flag matrix (canonical reference for `KyuzenCompiler.cmake`)

| Flag set | `--target` | `-O` | `-mcmodel` | SSE off | `-std` | exceptions/RTTI | `-nostdlib` |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Kernel `CFLAGS` | `x86_64-pc-none-elf` | `2` | `kernel` | ✔ | `c11` | n/a | ✔ |
| lwIP `LWIP_CFLAGS` | = kernel | `2` | `kernel` | ✔ | `c11` | n/a | ✔ |
| App C `CFLAGS_APP` | `x86_64-pc-none-elf` | `0` | — | ✔ | (default) | n/a | ✔ |
| App C lib `CFLAGS_LIB` | `x86_64-pc-none-elf` | `2` | — | ✔ | (default) | n/a | ✔ |
| Widget C++ `CXXFLAGS_LIB` | `x86_64-pc-none-elf` | `2` | — | ✔ | `c++17` | off/off | ✔ |
| SDK C `SDK_CFLAGS` | `x86_64-pc-none-elf` | `2` | — | ✔ | (default) | n/a | ✔ |
| SDK C++ `SDK_CXXFLAGS` | `x86_64-pc-none-elf` | `2` | — | ✔ | `c++17` | off/off | ✔ (`-nostdinc++`) |
| libc port | `x86_64-pc-none-elf` | `2` | — | ✔ | `gnu++17` | off/off | ✔ |
| FreeType | `x86_64-pc-none-elf` | `2` | — | ✔ | `c11` | n/a | ✔ |
| Lexbor | `x86_64-pc-none-elf` | `2` | — | ✔ | `c11` | n/a | ✔ |
| BearSSL OS | `x86_64-pc-none-elf` | `2` | — | ✔ | `c11` | n/a | ✔ |
| BearSSL host | (host) | `1` | — | — | (default) | n/a | — |
| Host tests | (host) | `1`/`2` | — | — | `c++17` where C++ | — | — |

## Appendix C — Object namespace map

```text
build/obj/…              kernel objects        (CFLAGS, -mcmodel=kernel)
build/obj/user/…         user-space objects    (CFLAGS_APP / CFLAGS_LIB)
build/obj/libdesktop/…   libdesktop objects
build/obj/desktop-<app>/… desktop implementation objects
build/obj/filemanager/…  fileman objects
build/obj/settings/…     settings objects
build/obj/taskmgr/…      taskmgr objects
build/obj/browser/…      browser C++ objects
build/obj/browser-tls/…  browser BearSSL objects
build/libc/…             LLVM libc + port + phase apps
build/lexbor-host/…      host Lexbor
build/host-tls/bearssl/… host BearSSL
build/sdk/c/…            C SDK stage
build/sdk/cpp/…          C++ SDK stage
build/iso_root/…         ISO staging
build/apps/…             final ELFs
```

`libs/gui/color/src/*.c` appears in **both** `build/obj/…` (kernel) and
`build/obj/user/…` (user-space) — the only source compiled for both domains.
