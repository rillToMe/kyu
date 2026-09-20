# GPU Architecture — KyuzenOS 2D Hardware Acceleration

**READ /roadmap/Progress/2d_accelleration.md**
## 1. Subsystem Boundaries

```
Application (user_apps/)
    |
   libui (libs/widget/)
    |
   KWM (kernel/gfx/kwm.c)
    |
   Compositor (kernel/gfx/compositor.c)
    |
   Damage Regions (display.h DirtyRegionList)
    |
   GHAL (graphics/ghal.c)
  /     \
software  virtio-gpu   intel-backend (NEW)
backend   backend      (graphics/backend/intel/intel_*)
  |         |              |
  fb_ptr   VirtIO MMIO  Intel MMIO (BAR0)
```

### Ownership Rules

| Subsystem | Owns | Does NOT touch |
|---|---|---|
| KWM | Window lifecycle, z-order, focus, input routing | Pixels, GPU commands, surfaces |
| Compositor | Damage regions, base-blit optimization, opaque occlusion, chrome rendering | GPU registers, MMIO, hardware specifics |
| GHAL | Backend selection, vtable dispatch, NULL-safe wrappers | MMIO, registers, hardware specifics |
| Software backend | CPU memcpy/loop rendering | Any hardware |
| VirtIO-GPU backend | VirtIO commands, virtqueues, scanout | Intel registers |
| Intel backend (NEW) | PCI detection, MMIO, blitter, GTT, fences | KWM, windows, compositor logic |

### Key Invariants

1. **Compositor never calls backend directly** — always through `ghal_*()` functions.
2. **GPU code never knows about windows** — only buffers, surfaces, rectangles, fences.
3. **Intel-specific code stays inside `graphics/backend/intel/intel_*.c`** — never leak to GHAL or compositor.
4. **Software backend never fails** — wraps Limine framebuffer, zero allocation for scanout.

---

## 2. Buffer Lifetime Rules

### DisplayBuffer (include/display.h)

- Allocated by KWM (`kwm_create_window`) or compositor.
- `owns_pixels=1`: `display_buffer_destroy()` frees the pixel array.
- `owns_pixels=0`: borrows external memory (e.g., `fb_ptr`, GPU backing pages).
- Canvas stride == width (KWM invariant). Framebuffer stride == `pitch_bytes/4`.

### GHAL Surface (graphics/ghal.h, opaque)

- Created via `ghal_surface_create()` (private buffer) or `ghal_surface_create_scanout()` (wraps HW).
- `surface_upload()` copies system RAM → surface backing memory.
- `surface_destroy()` frees all resources (backing pages, device handles).
- Scanout surface (`owns_pixels=0`): upload writes directly to screen, present is no-op.

### Intel GPU Buffer (NEW, Phase 5-6)

- Physical pages allocated via `pmm_alloc_page()`.
- Mapped into GPU address space (GTT) for blitter access.
- Mapped into CPU address space (HHDM) for CPU read/write.
- Lifetime: created by `gpu_buffer_create()`, destroyed by `gpu_buffer_destroy()`.
- **Never share GPU virtual address with CPU virtual address** — they are separate address spaces.

---

## 3. Synchronization Model

### Current State (pre-Intel)

- **Single-compositor context**: `compositor_flush()` runs from timer IRQ, single-threaded composition.
- **VirtIO-GPU fence**: `ghal_present_fence()` → `ghal_fence_wait()` provides frame-to-frame backpressure.
- **No GPU interrupts**: VirtIO-GPU uses polling (`virtio_gpu_dev_fence_wait()`).

### Intel Backend Target (Phase 13-14, as built)

- **Phase 13**: Synchronous submit+wait. Fence = seqno via MI_STORE to a
  status page; wait = sleep (`sti;hlt`, wake on any IRQ) with pause fallback.
- **Phase 14 (split)**: lock-free ISR body (`intel_irq_handler`: volatile
  snapshot + counter, no MMIO) shared with the sleep-wait path. IMR unmask +
  IDT vector + EOI deferred — unmasking INTx with no installed vector would
  #GP on real HW, and QEMU has no iGPU to validate against.
- **SMP safety**: one submission owner (`g_bcs_lock`); staging is
  caller-owned stack; ring-full degrades to CPU fallback. Verified -smp 8.

### Locking Hierarchy

```
g_dirty_lock          (compositor → dirty regions)
kwm_lock              (KWM → window state)
ghal_lock (g_lock)    (GHAL → backend registry/selection)
gpu_lock (NEW)        (Intel backend → MMIO submission)
```

---

## 4. Fallback Behavior

| Failure | Response |
|---|---|
| No Intel GPU detected (PCI scan) | Software backend active, no error |
| Intel GPU BAR mapping fails | Software backend, log warning |
| MMIO access verification fails | Software backend, log warning |
| GPU command submission fails | Return error to compositor, software path |
| Fence timeout | Bounded wait expires, CPU fallback, continue |
| GPU reset needed | N/A — legacy BCS ring has no reset mechanism |
| Unsupported Intel generation | Software backend with diagnostic output |

**Critical rule**: Graphics failure must never crash the kernel. The software fallback is always available.

---

## 5. Planned Driver Interface

### Phase 1: GPU HAL (already exists as GHAL)

The existing `ghal_backend_ops_t` vtable is the correct interface. The Intel backend implements this vtable.

```c
// Graphics backend registration — same pattern as virtio-gpu
extern const ghal_backend_ops_t intel_backend_ops;

// In graphics/backend/intel/intel_init.c:
const ghal_backend_ops_t intel_backend_ops = {
    .name           = "intel",
    .capabilities   = GHAL_CAP_PARTIAL_FLUSH,  // Phase 15+ may add HW_CURSOR
    .init           = intel_init,
    .shutdown       = intel_shutdown,
    .surface_create = intel_surface_create,
    .surface_destroy= intel_surface_destroy,
    .surface_create_scanout = intel_surface_create_scanout, // wraps fb_ptr
    .surface_upload = intel_surface_upload,
    .fill_rect      = intel_fill_rect,   // HW on GPU surfaces, CPU fallback
    .blit           = intel_blit,        // HW 1:1 on GPU surfaces, CPU fallback
    .present        = intel_present,     // no-op (synchronous model)
    .cursor_update  = NULL,          // no HW cursor (2D scope)
    .cursor_move    = NULL,
    .mode_get       = intel_mode_get,
    .mode_enumerate = intel_mode_enumerate,
    .mode_set       = NULL,
    .mode_changed   = NULL,
    .acceleration_enabled = intel_accel_enabled, // BCS-live (Phase 15)
    .engine_name    = intel_engine_name,         // "BCS"
};
```

### Backend Selection Order (modified ghal_init)

```
1. virtio-gpu  (tried first — existing)
2. intel       (NEW — PCI detection + MMIO init)
3. software    (fallback — never fails)
```

### Intel Backend Internal Structure (as built)

```
graphics/backend/
    intel_init.c        — PCI detection, MMIO verify, GHAL vtable (HW+CPU ops)
    intel_backend.h     — vtable extern, accessors, SMP lock protocol
    intel_regs.h        — register + opcode definitions (generation-specific)
    intel_mmio.c        — MMIO read/write helpers
    intel_gpu_alloc.c   — GPU buffer pool (PMM + HHDM + GTT vaddr)
    intel_gtt.c         — GTT page table (vaddr-indexed PTEs)
    intel_bcs.c         — BCS ring: init, submit, idle-wait
    intel_cmd.c         — staging + MI/XY emit + builder self-test
    intel_fence.c       — seqno fences, submit, spin/sleep wait
    intel_irq.c         — lock-free ISR body + wakeup counter
    intel_rect.c        — rect/boundary validation gate + self-test
    intel_surface.c     — GPU-backed surface (gsurf) + self-test
    intel_bench.c       — TSC benchmark harness (always-on)
    intel_robust.c      — failure-path self-test (always-on)
    intel_test_copy.c / fill / blit — HW op tests (boot, non-fatal)
```

All Intel code lives under `graphics/backend/` — no new top-level directories.

### PCI Detection (Phase 3)

Uses existing PCI subsystem (`drivers/pci.c`). Scans for class=0x03, subclass=0x00 (VGA controller). Reads vendor/device/BARs. Does NOT hardcode device ID.

### MMIO Mapping (Phase 4)

Pattern from e1000/VirtIO-GPU drivers:
```c
extern uint64_t hhdm_offset;
volatile uint32_t* mmio = (volatile uint32_t*)(bar0_phys + hhdm_offset);
```

No explicit VMM mapping needed — HHDM provides direct 1:1 physical→virtual for RAM-mapped BARs.

---

## 6. Integration Points

### Where Intel Backend Hooks In

1. **`ghal_init()`** (ghal.c:51): Backend selection loop — add `&intel_backend_ops` between virtio and software.
2. **`ghal_register_backend()`** (ghal.c:31): Optional — if Intel backend self-registers instead of static array.
3. **`compositor_flush()`** (compositor.c:694): No changes needed — already goes through GHAL vtable.
4. **`kwm_update_window()`** (kwm.c:398): No changes needed — writes to `DisplayBuffer*`, GHAL handles acceleration.
5. **PCI probe** (kernel.c): `pci_probe()` already runs at boot — Intel GPU will appear in scan output.

### Where Intel Backend Must NOT Hook

- `kwm_create_window()` — window management stays CPU-side.
- `compositor_flush()` internals — composition logic stays CPU-side.
- `display_boot_init()` — framebuffer capture from Limine is unchanged.
- `timer_callbacks_init()` — compositor flush timing is unchanged.

### Build System

Add `graphics/backend` to `SRC_DIRS` (already there) — new `.c` files in that directory are auto-discovered by the Makefile's wildcard. No Makefile changes needed for new files in existing directories.

---

## 7. Pixel Format

**XRGB8888** — 32bpp, little-endian `0x00RRGGBB`.

- High byte (bits 24-31): opacity mask. `0x00` = transparent, non-zero = opaque.
- Intel blitter operates on this format natively.
- ARGB8888 used only for hardware cursor (64x64).

---

## 8. Validation Criteria (Phase 0 Exit)

- [x] All subsystems identified and documented
- [x] Integration points mapped
- [x] No code changes to KWM, compositor, or existing GHAL
- [x] Fallback behavior defined
- [x] Buffer lifetime rules documented
- [x] Synchronization model documented
- [x] Driver interface planned
- [x] PCI GPU detection works (Phase 3)
- [x] MMIO access verified (Phase 4)

---

## 9. Gen12 Alder Lake extension (AL-1–AL-20)

Target HW: `VEN_8086 DEV_468B REV_0C` (Alder Lake-S UHD GT1, Gen12 —
confirmed via LKDDb `i915_pci.c` match + host WMI; BAR0 addr/size
printable only on a physical boot, still pending).

### Dispatch (frozen GHAL API, `ghal.*` zero-diff)

```
Intel Gen8–11 → legacy BCS ring path (frozen)
Intel Gen12   → Gen12 execlist path (intel_gen12_*, new files only)
otherwise     → software backend
```

GHAL fill/blit try Gen12 only when the boot STORE proved the engine
live (`gen12_is_live`); any submit/wait failure clears live
(fail-fast, no per-frame stalls). Legacy `intel_gtt_map_pages()`
is byte-identical to frozen — Gen12 separation is by new files,
never by editing legacy.

### New modules (`graphics/backend/intel/intel_gen12_*`)

`engine` (BCS0 discovery from i915 tables) → `vm` (GGTT-only minimum,
window validator) → `ctx` (4-page LRC, gen12_xcs image) → `batch`
(verified XY builders) → `fence` (own status page + seqno) →
`ppgtt` (PD/PT alias of GGTT) → `submit` (single owner: FW + ELSP) →
`test_copy/fill/blit` (5 cases each) → `bench` → `ghal` (GHAL-level
check, public API only) → `robust`/`fault` (AL-16/17 suites).

### GGTT reservation map (2MB window)

```
0x00000 legacy BCS ring (32KB) | 0x10000 legacy status
0x20000 gen12 context (16KB)    | 0x24000 gen12 status
0x25000 gen12 ring (16KB)       | 0x100000+ buffer bump area
```

### Status: implemented / verified / SKIP / BLOCKED

| Item | Status |
|---|---|
| AL-1 discovery, AL-2 MMIO+BAR, AL-3 engine table | implemented, build-verified |
| AL-4 GGTT-only determination (i915 `intel_lrc.c`) | implemented |
| AL-5 LRC image (gen12_xcs + CTX slots, pure-CPU validated) | implemented |
| AL-6 XY layouts (IGT + i915 proven, NOT Phase-8 reuse) | implemented, struct-tested |
| AL-7 fence, AL-12 bench, AL-13 dispatch, AL-14 GHAL check | implemented |
| AL-15 single-owner commit + ordering proof | implemented (code); -smp 8 run BLOCKED (no QEMU in env) |
| AL-16/17 robust + fault suites | implemented, run every boot |
| AL-18 final diag (`submission: gen12`, enabled iff live) | implemented |
| HW PASS (STORE/COPY/FILL/BLIT/fence/SMP/bench numbers) | BLOCKED — needs physical Alder Lake boot |
| QEMU verification | SKIP — no iGPU, no qemu/cargo in this env |

### Known legacy findings (reported, NOT silently fixed)

1. Legacy ring regs (`0x12040/60/64/68`) ≠ i915
   `RING_xxx(BLT_RING_BASE)` (`0x12038/30/34/3c`) — Gen8–11 path
   likely never drove HW (fail-closed via CTL-reject).
2. Phase-8 XY builders (12/8 DW, depth 2) ≠ HW-proven layouts
   (10/7 DW, depth 3, coords-first) — Gen12 uses corrected copies.
Both need Gen8–11 HW to validate any fix; frozen until then.
