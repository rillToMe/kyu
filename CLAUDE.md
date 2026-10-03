# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Binding Rules (read before changing anything)

`.rules/` holds the mandatory development rules. They are **binding**: violating
one is a bug, not a style disagreement. This file (`CLAUDE.md`) is orientation -
when the two disagree, `.rules/` wins.

Read the file that matches what you are about to touch. Do not read all of them
up front; pick by task.

| File | Read it when you are about to... |
|---|---|
| [`RULES.md`](.rules/RULES.md) | start any task - entry point, general principles, AI agent rules |
| [`STYLE_GUIDE.md`](.rules/STYLE_GUIDE.md) | write or refactor code - clean code, comments (keep them short; no WHAT, only non-obvious WHY), naming, performance |
| [`ARCHITECTURE.md`](.rules/ARCHITECTURE.md) | touch kernel APIs, drivers, error handling, memory safety, synchronization, logging, or refactor an existing subsystem |
| [`UI.md`](.rules/UI.md) | change anything under `libs/gui/widget/`, `include/libui*.h`, `apps/`, `system/desktop/`, or `ui/xml/` |
| [`BUILD.md`](.rules/BUILD.md) | finish a task - build verification, testing, the wrong-clang trap |
| [`DOCUMENTATION.md`](.rules/DOCUMENTATION.md) | change behavior - what docs must be updated in the same commit |
| [`REVIEW_CHECKLIST.md`](.rules/REVIEW_CHECKLIST.md) | declare a task done - completion checklist + the UI checklist |

Commit, branch, and pull request rules live at the repository root in
[`RULES.md`](RULES.md), not in `.rules/`.

### Auditing compliance (how to check, not just read)

The rules are written to be checkable. Before claiming a UI change is done,
grep for the anti-patterns instead of eyeballing the screen. Run these from the
repo root; every command below is verified to produce meaningful output.

```sh
# Hardcoded colors outside the token owner. Expected hits ONLY in:
#   theme.hpp      - the token table itself (it owns the palette)
#   xml_inflate.cpp - parses theme-custom from XML
#   textedit.hpp:54 - KNOWN deviation UI-25.1 (terminal prompt color)
grep -rn 'color_hex(\|COLOR_HEX(' libs/gui/widget/src libs/gui/widget/include \
  | grep -v '/theme/'

# Visual attributes in XML. Must be EMPTY - the inflater rejects them (UI-18.2).
grep -rn 'padding=\|radius=\|shadow=\|color=' ui/xml/

# Off-scale spacing in XML. Only spacing="0" (collapse) is legitimate (UI-3.4).
grep -rn 'spacing="' ui/xml/

# Widgets branching on theme mode to pick a color. Must be EMPTY (UI-2.4);
# the only legitimate hits are ABI parsing and settings deserialization.
grep -rn 'UI_THEME_DARK\|UI_THEME_LIGHT' libs/gui/widget/src libs/gui/widget/include \
  | grep -v 'theme.hpp\|colors.hpp\|xml_inflate\|window.hpp'

# Gradients in new UI. Must be EMPTY outside painter.hpp itself (UI-10.4).
grep -rn 'vgrad\|rrect_grad' libs/gui/widget/src libs/gui/widget/include \
  | grep -v 'painter.hpp'
```

`ui/xml/` must contain real files that the build embeds (`kyuzen_embed_xml` in
`apps/CMakeLists.txt`). An XML document that only exists as a string literal in
a `.cpp` violates UI-18.4 - check with `grep -rn 'ui_xml_parse' apps/` and
confirm the argument is a generated `ui_xml_*` symbol, not an inline string.

`.rules/UI.md` §25 lists the KNOWN, current deviations. If a grep above hits one
of those, it is a known bug, not a precedent - do not copy it, and do not
"fix" it as a drive-by in an unrelated commit.

## Language Policy (mandatory)

Development is restricted to **C, C++, and Rust** — the languages already used in this repo. Do not introduce other languages or compilers.

- Kernel, drivers, filesystem, host tools (`tools/`), and host tests (`tests/host/`): **C** (compiled with `clang`, see Build & Run).
- Userspace apps: **C** (`apps/`, `system/` CLI, `tests/` ELF uji) or **C++** (`apps/filemanager|gallery|imageview`, `system/desktop`), plus **Rust** (`rust/`, no_std userspace, built with `cargo`). Core user-space libs live in `libs/core/` (`userlib.c`, `libgui.c`, ...).
- Never use other compilers/languages for building or testing (e.g. no `zig cc`, no Go, no Python for project code).
- Exception: build-orchestration tooling only (e.g. `python -m compiledb` for `compile_commands.json`) is not project code and stays as-is.

## Build & Run

Toolchain lives in `E:\Tools\msys2`. **Use `./build.sh`** — it locates MSYS2 and
sets PATH correctly no matter which shell you started in.

```sh
./build.sh setup      # fetch llvm-project (~137 MB sparse; skipped if present)
./build.sh            # kernel + libs + apps
./build.sh iso        # + Rust + ISO staging → build/target/boot_image.iso
./build.sh run        # boot in QEMU (8 CPUs, 1G RAM, e1000 NIC, disk.img)
./build.sh test       # host tests (separate build tree)
./build.sh mkfs       # KyuzenFS disk formatter (host tool)
./build.sh clean      # remove build/ entirely
./build.sh <target>   # any ninja target (kyuzen-kernel, kyuzen-calc, ...)
```

`llvm-project` is ~1.9 GB and is NOT committed — the SDKs are built from its
`libc`/`libcxx`. `setup` clones it sparsely (only what is compiled) at the
pinned tag. Run it once on a fresh clone; it is safe to re-run.

Why the script rather than plain `cmake`/`ninja`: MSYS2 ships two clang builds
and this project needs tools from both, in a combination no single PATH order
produces. `clang`/`nasm`/`xorriso` must come from `usr/bin` (LLVM 21 — the
parity baseline), while `cmake`/`ninja`/`ctest` must come from `clang64/bin`,
because the `/usr/bin` ones are Cygwin binaries that write POSIX paths native
Ninja cannot read. Getting this wrong either fails confusingly (`/usr/bin/cmake`)
or produces a byte-different kernel with no error at all (`clang64/bin/clang`,
LLVM 22). See `docs/development/cmake-migration.md`.

All build artifacts go under `build/` (gitignored) and **nothing is written into
the source tree**, so `rm -rf build` is a complete reset:

```text
build/target/        kernel + libs + apps + ISO
  bin/myos.bin       the kernel (+ a byte-identical compat copy at build/target/myos.bin)
  apps/*.elf         user-space ELFs
  iso_root/          ISO staging
  boot_image.iso     the hybrid BIOS+UEFI image
build/host/          host-side unit tests
build/target/tools/  generated compiler wrappers (depfile normalisation)
```

Incremental builds are exact: `ninja` recompiles only what changed, and a no-op
build reports `no work to do` in 0 steps.

Host-side unit tests (17 tests, run with clang, no QEMU needed). Many compile
the REAL production sources against mocks rather than copies of the logic, so a
bug in the kernel or library code shows up here:

```sh
./build.sh test
./build.sh test && ctest --test-dir build/host -R test-color   # a single test
```

**Running `ctest` directly fails with `0xc0000135` (DLL not found) unless
`E:\Tools\msys2\clang64\bin` is on `PATH`.** The host test binaries link against
clang64 runtime DLLs, so a bare `ctest` from a plain PowerShell window reports
`BAD_COMMAND` or `Exit code 0xc0000135` for every test - that is a missing DLL,
NOT a test failure, and NOT a reason to disable a test. Either go through
`./build.sh test` (which sets PATH for you) or prefix PATH yourself:

```powershell
$env:PATH = "E:\Tools\msys2\clang64\bin;E:\Tools\msys2\usr\bin;" + $env:PATH
& "E:\Tools\msys2\clang64\bin\ctest.exe" --test-dir build/host -R libui
```

Calling `ctest` from inside `bash -lc` also fails differently: MSYS rewrites the
absolute test path and you get `BAD_COMMAND` on every test. Use native
PowerShell for `ctest`, or use `./build.sh test`.

Most other tests run inside QEMU as kernel tasks.

jika ingin memasukin desktop nya atau ingin membuka login nya:
user: root
pw: 1
jadi tidak perlu lagi bikin disk baru untuk coba memasuki desktop
## Architecture

64-bit higher-half monolithic kernel. Limine bootloader (protocol rev 3). Kernel mapped above `0xFFFFFFFF80000000`. HHDM offset added to all physical addresses before dereferencing.

### Memory layers

| Layer | Files | Notes |
|---|---|---|
| PMM | `kernel/mm/pmm.c`, `include/pmm.h` | Bitmap allocator. `phys_addr_t` ≠ virtual pointer — add `hhdm_offset` before use. `pmm_owns_page()` skips Limine boot pages. |
| VMM | `kernel/mm/paging.c`, `include/paging.h` | Per-task PML4. `vmm_user_pml4` extern routes ELF-load allocs into the right address space. `vmm_map_page_kernel` keeps heap pages visible across AS switches. |
| Heap | `kernel/mm/heap.c`, `include/heap.h` | Linked-list allocator. `heap_block_t` header has magic + canary — corruption detected on free. `kmalloc` / `kfree` / `krealloc`. |

### Scheduler (`kernel/task.c`, `include/task.h`)

- Preemptive, SMP-aware. `MAX_TASKS=8`, `TASK_STACK_SIZE=8192`.
- Priority 0–3 with aging to prevent starvation.
- `registers_t` layout is load-bearing — must match `isr_macro.inc` PUSHA64 frame exactly.
- **Two-phase block API**: `block_prepare()` + `block_park()` under the object's lock closes the lost-wakeup window. Never call `block_park()` without a preceding `block_prepare()`.
- `smp_current_task_id()` for per-CPU task identity.

### VFS & storage

- `fs_node_t` vtable (`fs/vfs.c`, `include/fs.h`) abstracts device/FS operations.
- Per-task fd table in `kernel/vfs_fd.c` (max 16 fds, keyed by task id). File I/O is vnode-based streaming through the 4KB LRU block cache (`kernel/fs/bcache.c`) — no whole-file RAM buffer, writes are in-place.
- KyuzenFS V4 is extent-based and sits on the ATA driver via `ata_read_block4k`/`ata_write_block4k`. Layout: `include/kyuzenfs_v4.h`; vnode interface: `include/vnode.h`; public shim API for callers: `include/kyuzenfs.h`. Implementation is split by responsibility under `kernel/fs/` (contract: `kernel/fs/kfs_internal.h`):
  - `kfs_super.c` — global FS state, superblock, layout, mount/format, global sync
  - `kfs_balloc.c` — bitmap helpers + inode/block (run) allocation
  - `kfs_inode.c` — inode disk I/O + canonical inode cache (`ino_get/put`)
  - `kfs_extent.c` — extent engine + block-level file read/write + truncate
  - `kfs_dir.c` — dirent ops, path walk, create/unlink child, vnode wrap
  - `kfs_vnode.c` — `struct vnode_ops` implementation (used by `vfs_fd.c`)
  - `kfs_shim.c` — legacy `kfs_*` API for V3-style callers
- Periodic sync runs on a timer; disk is flushed on poweroff/reboot.
- Legacy V3 sources are archived in `legacy/` — do not build on them.
- ATA `IDENTIFY` (`ata_get_total_sectors()`, `drivers/ata.c`) needs the full protocol: wait BSY, select drive, **400 ns delay**, send `0xEC`, **400 ns delay**, then poll DRQ with ERR/DF checks. Without the delays the first status read returns `0x00` and the function reports "no drive" although normal sector I/O works — that silently disabled the panic crashdump (`[CRASHDUMP] disk terlalu kecil`) and made `kfs_format()` fall back to a hardcoded size. `ata_wait_bsy()`/`ata_wait_drq()` already exist — use them instead of open `while` loops (no timeouts = hang on real hardware).
- Format disk images with the host tool: build it once with
  `ninja -C build/target mkfs.kyuzenfs`, then `./mkfs.kyuzenfs disk.img` (respects an existing file's size).

### Network

lwIP `NO_SYS=1` integrated in `third_party/net/`. e1000 driver in `drivers/net/e1000/`. Only three kernel files get `LWIP_CFLAGS`: `net_init.c`, `net_ping.c`, `net_socket.c` — do not add lwIP includes elsewhere.

### Userspace

ELF loader (`kernel/proc/elf.c`) maps binaries into per-task address spaces. Syscalls via `int 0x80` (`kernel/syscall.c`). User apps in `user_apps/` are built as separate ELFs.

### Display

Dirty-region compositor (`kernel/display.c`). All writes to `base_canvas` must mark the region dirty — the presenter only flushes dirty rects.

### Panic / BSOD & crash safety

Modules (all zero-dynamic-allocation, no locks — the heap and scheduler are not trusted here):

| File | Responsibility |
|---|---|
| `kernel/panic/` | BSOD module split in four: `panic.c` (orchestrator: lockdown, entry points, persistence, operator loop), `panic_draw.c` (draw target = device scanout else framebuffer, primitives, layout), `panic_hw.c` (IRQ-free PIT2/PS-2 polling, IRQ1/IRQ12 masking, reset/reboot/shutdown/freeze), `panic_explain.c` (fault verdict, RIP class, backtrace). During lockdown the panic loop OWNS the PS/2 controller: any IRQ-driven reader of ports 0x60/0x64 (currently `drivers/keyboard.c`, `drivers/mouse.c`) must early-return on `panic_is_locked()` BEFORE touching the data port, otherwise it swallows the scancodes that drive `[R]`/`[S]` |
| `kernel/debug/panic_log.c` | ringkas crash log in reserved RAM (survives warm reboot) |
| `kernel/debug/crash_archive.c` | publishes the last raw dump as `/crash-report.txt` (readable file) |
| `kernel/debug/crashdump.c` | raw 4KB snapshot to the disk tail (polling ATA, no IRQ) |
| `drivers/acpi.c` | minimal FADT parse — power-off (S5) for `[S]` |

- `exception_handler()` and `kernel_panic()` draw straight to the framebuffer (no allocator) and mirror a summary to COM1 (`-serial stdio` / `./build.sh run-wd`).
- **Lockdown**: both call `panic_lockdown()` first (`include/panic.h`). `compositor_flush()`, `mouse_handler()`, and the HUD/cursor timer callbacks early-return while `panic_is_locked()` — otherwise SMP redraws (e.g. moving the mouse) paint over the BSOD. The only scanout writer is `compositor_flush()` (all callers, incl. `kprint.c` and `kernel_userlib.c`, are covered by its internal guard), so this is sufficient.
- **Hardware cursor**: `compositor_flush()` no longer runs after panic, so `cb_flush` calls `compositor_panic_cursor_off()` to move the device cursor plane off-screen once — otherwise it stays frozen floating over the BSOD.
- **No IRQ in the panic path.** Everything runs under `cli`, so the panic code must never depend on IRQ0/IRQ1 and must never `sti` back on. Time is measured by polling **PIT channel 2** (speaker channel, reprogrammed to 100 Hz mode 3 — it generates no IRQ) and keys are read by polling the **PS/2 controller** (`0x64`/`0x60`). Do not reintroduce `timer_get_ms()`/`panic_sti()` in this path: if the timer IRQ is dead (common when the fault is in the scheduler or after `cli`), the countdown would hang forever.
- **NO auto-reboot (by request)**: the handler stops at the BSOD and waits for the operator. There is no countdown and no `PANIC_REBOOT_DELAY_MS`; with no key pressed the loop just keeps polling. Rationale: an automatic reboot destroys the evidence before it is read and (in QEMU) produces a boot loop that hides the original cause. To compensate for the lost safety net, `panic_interactive()` does **one best-effort FS sync** on entry via `kfs_sync_all_try()` (never a blocking `kfs_sync_all()`), and the info row says whether it succeeded (`TIDAK ada reboot otomatis ...` / `... sync FS dilewati (lock dipakai)`). Do not reintroduce an automatic reset/countdown.
- **Interactive actions** (hint row is the bottom line of the BSOD, and there are exactly two): `[R]` reboot, `[S]` shutdown (`kfs_sync_all()` → `acpi_poweroff_raw()` (SLP_TYP from `_S5` in DSDT, fallback 5) → QEMU/Bochs/VBox emulator ports → freeze fallback). Reboot chain is industry-ordered everywhere (`system_reboot()` + `panic_reset_hw()`): FADT RESET_REG → `0xCF9` (`0x02` then `0x06`) → bounded 8042 `0xFE` (unbounded wait would hang forever on laptops without a PS/2 controller) → triple fault. Only these keys end the loop. A `[D]` freeze key used to exist and was removed by request — with no auto-reboot, doing nothing *is* the freeze, so the key added nothing but a third line of UI. `panic_action_freeze()` is gone with it; keep `panic_freeze()` (static) for the nested-panic and failed-power-off paths. `panic_set_reboot_hook()` overrides the reset for host tests.
- **PS/2 polling must consume MOUSE bytes too.** `panic_ps2_read()` reads port `0x60` whenever the status has OBF set, discards the byte when AUXB is set, and `panic_read_key()` keeps looking. An earlier version returned early on AUXB *without* reading — the 8042 output buffer then stayed full and keyboard scancodes never arrived, so **`[R]`/`[S]` looked dead exactly after the mouse was moved on the BSOD screen** (reproduced in QEMU: BSOD → `mouse_move` → `sendkey r` = no reaction; before the mouse, same key worked). One byte read per poll, mouse bytes skipped, never block. Host test model: `g_panic_test_aux` = mouse bytes queued in front of `g_panic_test_key` (`test_mouse_bytes_do_not_block_keys`); note `panic_keys_drain()` only flushes the aux queue in the host model — the queued key represents a press *after* the panic and must survive.
- **Nested-panic guard**: `g_panic_active` is set at handler entry; a second panic while one is running (e.g. the crashdump I/O faults) aborts the dump via `crashdump_abort()` and immediately freezes instead of drawing/writing again. The `return` after `panic_interactive()` is unreachable in the kernel (every action ends in reboot/freeze) — it exists for the host test.
- **Persistence**: RAM log (`panic_log_init()` right after `init_heap()` on the *first* PMM page — deterministic address, so it is found again after a warm reboot; `panic_check_previous_log()` reports it once at boot) plus the on-disk crashdump. Both run inside `panic_persist()`.
- **Crash report as a real FILE** (`kernel/debug/crash_archive.c`): the raw 4 KB dump in the disk tail sectors is published to **`/crash-report.txt`** at the *next* boot (`crash_archive_publish()`, called once after `kfs_init()` + `crashdump_init()` in `kernel/kernel.c`). Users open it from fileman/viewer/notepad or `baca /crash-report.txt` — the crash record must not be terminal/serial-only. Three invariants here: (a) publishing runs in **normal boot context, never in the panic path** (writing a file needs the FS lock + bcache, exactly the locks the faulting CPU may hold); (b) it is **idempotent** — the report is compared with the existing file and only rewritten when it differs (booting repeatedly must not keep writing the disk); (c) an invalid/absent dump is a silent no-op with one serial line, never a garbage file. `crashdump_read_last()` validates the payload checksum before anything is published. Root-level path is deliberate: `fileman` only lists `/`.
- **Desktop notification for a fresh crash** (`include/crash_notice.h` + syscall `SYS_CRASH_NOTICE 80`): `crash_archive_publish()` leaves a small record of what it did (`pending` / `crash_count` / `uptime_ms` / `vector` / `rip` / `cr2` / `task` / `path`) behind `crash_archive_notice()`; `user_apps/desktop.c` asks for it once at startup and, only when `pending == 1`, paints a card in the top-right corner naming the file and opens the File Manager when clicked (the card also times out after `NOTIF_MS`). `pending` is 1 **only on the boot that actually (re)wrote the file**, so a normal boot never nags the user — that is the point of the flag, not a bug. The ABI struct lives in the shared header on purpose: the kernel and the user app must agree byte-for-byte, so never inline a copy of `crash_notice_t` in either side. **The card must never feel stuck**: it closes after `NOTIF_MS` (8 s, asserted to stay in the 3–15 s range) *or* on the first click anywhere — a click on the card dismisses it and opens the File Manager (the click is consumed), a click elsewhere only dismisses it and still performs its normal action (icon/taskbar clicks must not feel dead). Both paths go through `notice_close()` so the flag and the full-screen repaint request stay in one place; the card has no dirty-rect of its own, so erasing it is always a full `render()` (that is what `g_notice_repaint` is for).
- **Order is DRAW FIRST, PERSIST AFTER** (`exception_handler()` and `kernel_panic()` both): finish the BSOD on screen, then write the RAM log + crashdump, then enter the keys loop. `panic_persist()` touches the disk (ATA) and reserved RAM — if it runs before the screen, any slow/failing hardware there makes the machine look like it *froze with no panic* (desktop still on screen, no BSOD, no reboot). `test_screen_drawn_before_persist` locks this: the disk-write mock asserts the framebuffer already contains BSOD ink, and the serial stage markers must be ordered `[P1] lockdown → [P2] layar → [P3] persist → [P4] loop`.
- **Stage markers on COM1**: the panic path prints `[P1]..[P4]` (plus `[PANIC] handler masuk (cpu N)`) *before* anything can block. `./build.sh run` mirrors COM1 to `serial.log`, so a freeze can be localized from the last marker: no marker at all = the fault never reached the handler (deadlock in normal code, not a panic); `[P1]` only = blocked before/while drawing; `[P2]` only = blocked inside persistence; `[P3]/[P4]` = the handler is fine and the freeze is elsewhere. Keep these markers — they are the cheapest freezer diagnosis the OS has.
- **Serial TX is bounded in panic mode**: `serial_enter_panic_mode()` (called first in both entry points) switches `serial_putc()` to a spin **budget** (drops bytes once exhausted). Plain `serial_putc()` spins forever on the UART THRE bit, which on a stalled/absent serial backend hangs the handler before the BSOD appears — the same "freeze with no panic" symptom.
- **Nested panic is never silent**: besides the `crashdump_abort()` + freeze, `panic_overlay_banner()` paints a `PANIC BERSARANG ...` line at the top of the screen (BSOD stays readable underneath). Draw it with `C_BG` as background — a custom background colour breaks the `--dump` framebuffer decoder, which matches cells by "pixel != C_BG".
- **Bootloader addresses are already virtual**: `limine_rsdp_response.address` (and other Limine response pointers) come in **higher-half virtual form**, so adding `hhdm_offset` a second time produces a non-canonical pointer — this once caused an instant boot BSOD (`#GP(0)` on the first byte of the RSDP, `RDI = 0xFFFF0000_000F52E0`). `acpi_resolve_rsdp()` in `drivers/acpi.c` is the single place that decides (virtual → use as-is, physical → add HHDM) and it is unit-tested in `test/panic_test.c`. Pointers *inside* ACPI tables (XSDT entries, FADT) stay physical and still need `hhdm_offset`. Boot-order pitfall: this class of bug only shows up in QEMU, never in the host tests — when a boot panics instantly, run `./build.sh run-wd` and read the serial dump.
- **Crashdump area**: the last `KZFS_CRASHDUMP_SECTORS` (8 = 4 KB) sectors of the disk. KyuzenFS stops before that area, so a dump can never overwrite user data. The constant is duplicated in `include/kyuzenfs_v4.h`, `kernel/fs/kfs_super.c`, `tools/mkfs.kyuzenfs.c` (standalone tool) — keep them in sync, and `kernel/kernel.c` wires `crashdump_init(ata_get_total_sectors() - KZFS_CRASHDUMP_SECTORS, ...)`. The host test asserts the FS total-block count accounts for it.
- **Framebuffer = scanout (no double buffer)**: `panic_interactive()` draws its two static rows (info + hint keys) exactly **once**, before the loop, then only `panic_status_line()` may repaint its own row when an action is taken. Never redraw a row every loop iteration — that shows up as *flicker/tearing*.
- **NEVER read user memory from the panic path** (`p_backtrace`, `panic_bt_collect`). A ring-3 fault leaves `RSP`/`RBP` pointing into the **user** stack (real case: `CS=0x23`, `RSP=0x0BFBFFB8`, `RBP=1`); with SMAP on, dereferencing that from ring 0 raises a **second #PF**, which the nested-panic guard then turns into freeze — the BSOD stopped exactly at the `BACKTRACE` row. `paging_is_mapped_nolock()` is *not* a sufficient guard: the process PML4 legitimately makes user addresses look mapped. Use `panic_ptr_readable()` (canonical **and** `>= 0xFFFF8000_00000000`) before every read, and skip stack walking entirely when `(cs & 3) == 3`. `test_user_fault_never_reads_user_memory` locks both the drawn row and the crashdump payload.
- **`panic_cause_t.hint1/hint2` boleh NULL** — selalu cek sebelum dereference (`if (cause->hint1 && cause->hint1[0])`). Men-deref hint NULL pernah bikin segfault di jalur crashdump (bukan di jalur gambar, yang sudah mengeceknya).
- Diagnostics to keep when extending: fault decoding (error-code bits + likely cause), RIP/RSP/CR3 classification, stack backtrace, task name/kind, last syscall (`g_last_syscall_*` in `kernel/syscall.c`), PMM/CPU state.
- Command lanes: `./build.sh run` (window + `serial.log`), `./build.sh run-serial` (COM1 on stdio), `./build.sh run-wd` (COM1 to `serial.log`). `serial.log` survives the run — read its tail after a freeze.
- `ctest --test-dir build/host -R test-desktop` compiles `tests/host/unit/desktop_manifest_test.cpp`, which builds `system/desktop/*.cpp` with the FS/syscalls stubbed: it covers `discover_apps()`/manifests and the crash-notice lifecycle (card closes via click-inside (consumed, spawns the File Manager), click-outside (not consumed) or timeout; `notice_close()` is idempotent; `NOTIF_MS` stays human-scale). Keep this target wired up: the test silently failed to **link** for a while after `print`/`print_num`/`sys_crash_notice`/`gui_flush` entered `desktop.c`, and nothing noticed because the file had no build rule — using a new syscall in `desktop.c` means adding its stub here.
- **Widget toolkit ada di `libs/gui/widget/`** (dulu satu `apps/libui.cpp` 3.798 baris — lihat `DOCUMENTATION/design/widget-split.md`): `include/<layer>/` + `src/<layer>/` per layer, `core` (Theme/Painter/Widget) ← `primitives`/`layout` ← `containers`/`chrome`/`dialog` ← `window` (composition root) ← `abi/libui_abi.cpp` (satu-satunya tempat yang `reinterpret_cast` antara handle `ui_*` dan objek `ui::*`). Dua invarian wajib saat menambah widget: (a) layer bawah **tidak pernah** meng-include `window/window.hpp` — cukup `class Window;`, hanya `.cpp` yang benar-benar memanggil method `Window` yang meng-include-nya; (b) `operator new/delete` + `__cxa_pure_virtual` hanya boleh ada di SATU file, `src/runtime/runtime.cpp` (ODR), dan tidak ada global/static object dengan constructor non-trivial (ELF loader tidak menjalankan `.init_array`). `runtime/platform.hpp` adalah satu-satunya tempat yang membungkus `userlib.h`/`libgui.h`/libs-color dengan `extern "C"` (header-header itu tidak punya guard sendiri). Build: `apps/Makefile` mengompilasi `libs/gui/widget/src/*/*.cpp` + `abi/` jadi objek terpisah di `libs/gui/widget/build/` (di-gitignore) lalu link via `$(LIBUI_OBJ)`; test host nge-link sumber yang sama.
- **Aturan UI MENGIKAT ada di [`.rules/UI.md`](.rules/UI.md)** (bernomor `UI-n.m`, bahasa Inggris, "melanggar = bug"). Wajib dibaca sebelum menyentuh apa pun di `libs/gui/widget/`, `include/libui*.h`, `apps/`, `system/desktop/`, atau `ui/xml/`. Ringkasnya: semua nilai visual datang dari token di `theme/` (tidak ada warna/radius/jarak/ukuran font hardcoded di widget maupun app); XML hanya struktur & semantik (atribut visual `padding`/`radius`/`color`/`shadow` DITOLAK inflater); bayangan hanya `ELEV_POPUP`/`ELEV_DIALOG`; hover/press tidak dianimasikan; baris daftar tidak dibungkus kartu; maksimal satu tombol primary per permukaan; emoji bukan ikon. Checklist PR-nya di `.rules/UI.md` §24 dan `.rules/REVIEW_CHECKLIST.md`. Panduan cara-pakai (Indonesia) di `docs/design/gui/libui-design-system.md`; kalau berbeda, `UI.md` yang menang.
- **Teks harus IDEMPOTEN saat digambar ulang.** `kz_text_draw()` (libs/text) mengomposit coverage ke piksel yang SUDAH ada, jadi menggambar string yang sama dua kali membuat tepi antialias makin gelap - gejalanya "teks terlihat bold/membesar sampai kursor mendekat" (hover me-repaint latar dan mereset tinta). Pakai `kz_text_draw_on()` untuk apa pun yang bisa di-repaint, dan widget yang menggambar latarnya sendiri WAJIB memakai `Painter::surface_rect()` (bukan `rect()`) supaya teks dikomposit terhadap bg yang benar. Kontainer yang butuh anaknya ter-layout dulu memanggil `settle()` (arrange saja) - JANGAN menggambar anak dua kali per frame sebagai cara memicu layout; itulah yang dulu mengubah bug ini jadi kelihatan. Dikunci `tests/host/unit/text_test.c` (blok 3b).
- **TextEdit is the only editor widget** (`libs/gui/widget/include/editor/textedit.hpp`): notepad and terminal share it. Two rules when extending it: (a) **every text mutation goes through `apply_replace()`** — it is the single place that edits the buffer, records the undo op and fires the change callback, so typing/backspace/delete/paste/replace-all/undo/redo can never disagree about history or leave the modified flag stale; (b) **display rows are not document lines** once word wrap is on (`row_limit`/`disp_rows`/`disp_pos`/`disp_to_idx`) so scrolling, click-to-place-caret and the caret itself must all use those helpers — with `wrap == false` they collapse to the old line-based behaviour, which is what keeps the terminal/readonly path byte-identical. Undo is **operation-based** (del/ins pair per edit) instead of whole-buffer snapshots: 60 keystrokes = 60 undo steps without 60 copies of an 8K buffer. `ui_textedit_set_text()` clears the undo history on purpose (loading a file starts a new document). Selection comes from the widget (mouse drag + Shift+arrows/Home/End/PgUp/PgDn); cut/copy/paste/select-all/undo/redo are app-level calls, because window shortcuts are matched **before** widget keys — that is also why notepad's Ctrl+A is Select All while the widget still has its Emacs Ctrl+A/Ctrl+E line-start/end for the terminal.
- `ctest --test-dir build/host -R test-textedit` links the toolkit sources (`libs/gui/widget/src/*/*.cpp` + `libs/gui/widget/abi/libui_abi.cpp`) as-is against 20 stubbed symbols (syscalls + libgui + png) and drives the editor through the public C API: undo/redo (multi-step, redo branch dropped on a new edit), selection + clipboard, find/replace_all (longer/shorter replacement), word-wrap row arithmetic (10-column widget: 26 chars = 3 rows, break at the space, over-long word still cut), readonly refusals and the 8K cap. **This is the place to test editor logic** — QEMU/click testing can confirm a button works, but cannot cheaply check that wrap math or the undo ring is right.
- `ctest --test-dir build/host -R test-panic` compiles `kernel/panic/*.c` (orchestrator + draw + hw + explain), `panic_log.c`, `crashdump.c` and `drivers/acpi.c` with `-DPANIC_HOST_TEST` (privileged `cli`/`hlt`/CR2/CR3/in-out stubbed, clock = `g_panic_test_ms`, keys = `g_panic_test_key`) and mocks the framebuffer + ATA disk. Covered: fault verdicts, lockdown, "no auto-reboot" (loop exits only on a key), user-fault safety (no user-memory reads), draw-before-persist ordering, `[R]`/`[S]` (and that D no longer triggers anything), mouse-byte-does-not-block-key regression, crash-report publishing (file created / idempotent / no garbage file) plus its desktop-notice record (pending 0 before a publish, 1 after, with the path/task/vector fields filled) over a mocked KyuzenFS (incl. release-code/`0xE0`-prefix handling), RAM-log round-trip, crashdump header/payload/checksum + recursion guard, and the pure FADT parser (rev 1.0 / bad checksum / bogus port rejected). Layout regressions: build it manually and run `./test/panic_test --dump` — the framebuffer is decoded back into text with the real `font8x16`, so the BSOD hierarchy/alignment is visible without booting QEMU.
- Screen fonts are ASCII-only (`panic_draw_char` maps >127 to `?`), so **never** put em dashes / box-drawing characters in BSOD strings. Sections use `p_rule()` (label + 1px line) and the header uses `p_banner()` (two 2px rules) instead of `====`/`----` runs.
- Layout gotcha: a glyph is 16 px tall but rows are 18 px apart, so a rule/line drawn `cursor_y + 8` **crosses the text** (this once chopped the banner title). Put separators at least `PANIC_ROW_H` below the text they follow. Text starts at `PANIC_X = 50` (not a multiple of 8), so right-aligned text must be snapped back onto that grid (`tx -= (tx - PANIC_X) % 8`) or it renders in a different sub-cell column.
- `./test/panic_test --dump` decodes the framebuffer back into text: it searches the sub-cell x offset (0..7, needed because of `PANIC_X`) and prints `#` for rule rows. Keep the glyphs on one grid — offsets other than `PANIC_X % 8` show up as `?` in the dump.

---

## Behavioral guidelines

**Tradeoff:** These guidelines bias toward caution over speed. For trivial tasks, use judgment.

### 1. Think Before Coding

**Don't assume. Don't hide confusion. Surface tradeoffs.**

Before implementing:
- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them - don't pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.

### 2. Simplicity First

**Minimum code that solves the problem. Nothing speculative.**

- No features beyond what was asked.
- No abstractions for single-use code.
- No "flexibility" or "configurability" that wasn't requested.
- No error handling for impossible scenarios.
- If you write 200 lines and it could be 50, rewrite it.

Ask yourself: "Would a senior engineer say this is overcomplicated?" If yes, simplify.

### 3. Surgical Changes

**Touch only what you must. Clean up only your own mess.**

When editing existing code:
- Don't "improve" adjacent code, comments, or formatting.
- Don't refactor things that aren't broken.
- Match existing style, even if you'd do it differently.
- If you notice unrelated dead code, mention it - don't delete it.

When your changes create orphans:
- Remove imports/variables/functions that YOUR changes made unused.
- Don't remove pre-existing dead code unless asked.

The test: Every changed line should trace directly to the user's request.

### 4. Goal-Driven Execution

**Define success criteria. Loop until verified.**

Transform tasks into verifiable goals:
- "Add validation" → "Write tests for invalid inputs, then make them pass"
- "Fix the bug" → "Write a test that reproduces it, then make it pass"
- "Refactor X" → "Ensure tests pass before and after"

For multi-step tasks, state a brief plan:
```
1. [Step] → verify: [check]
2. [Step] → verify: [check]
3. [Step] → verify: [check]
```

Strong success criteria let you loop independently. Weak criteria ("make it work") require constant clarification.

---

**These guidelines are working if:** fewer unnecessary changes in diffs, fewer rewrites due to overcomplication, and clarifying questions come before implementation rather than after mistakes.
