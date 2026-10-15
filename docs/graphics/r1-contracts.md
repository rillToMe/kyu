# R1 Contracts — Surface, Synchronization, Native 2D API

Phase R1 (contract freeze + implementation). Status per item: implemented /
host-tested / QEMU-tested / hardware-verified — never assume one from another.

## 1. Audit baseline (verified against source)

| Component | Source ref | Status |
|---|---|---|
| DisplayBuffer + dirty mark + viewport | `kernel/display.c:25,57,80,90,194,283`, `include/display.h:107-163` | implemented, host-tested (`test-damage`) |
| fb mode/boot/alloc/draw | `kernel/gfx/fb.c:56,81,128,171,177,184,199,211` | implemented, host-tested (`test-damage`) |
| Compositor flush/cursor/panic | `kernel/gfx/compositor.c:76,706,726,929,947` | implemented, host-tested (`test-damage`) |
| KWM create/update/destroy/route | `kernel/gfx/kwm.c:165,242,287,322,438,513,584,704,845,864,943` | implemented, host-tested (`test-damage`) |
| KWM ABI 68 B | `include/kwm_abi.h:42,53` | frozen, unchanged by R1 |
| GHAL dispatch + present_checked + fence | `graphics/ghal.h:158-254`, `graphics/ghal.c:36-286` | implemented |
| Software backend | `graphics/backend/software.c:30-214` | implemented, host-tested |
| VirtIO backend + transport | `graphics/backend/virtio_gpu.c:66-505` | implemented, host-tested (`test-damage-driver`) |
| Intel Gen12 | `graphics/backend/intel/intel_gen12_*` | SKIP frozen, untouched |
| Damage path `gui_flush -> kwm_update -> dirty_region_mark -> cb_flush -> compositor_flush_frame -> base_blit -> composite -> ghal upload -> present_checked` | `libs/core/libgui.c:225`, `kernel/gfx/kwm.c:513`, `kernel/display.c:194`, `kernel/gfx/compositor.c:158,446,871,895` | preserved, no R1 change |

Thresholds unchanged: dirty 64, occlusion 8, opaque splits 4, split rects 12,
union inflation 2, windows 16, canvas 16 MiB. No tuning without data.

## 2. Surface contract (`include/kz_surface.h`)

Descriptor: width/height/stride_px/format/size_bytes/usage/id/generation/
owner. Formats mirror existing values (`XRGB8888=0`, `ARGB8888=1`); alpha:
XRGB = ignored top byte, ARGB = straight src-over. Coordinates integer
half-open, logical == physical pixels in R1 (no device scale in stride).

Lifecycle: `UNINIT -> CREATED -> WRITABLE -> READY -> IN_USE -> RELEASED`
(-> `WRITABLE` next frame); any live state -> `DESTROYED` terminal.
`kz_surface_transition()` enforces it (pure, host-tested). Stale refs
rejected by id+generation (`kz_surface_check_gen`). All dims validated with
overflow-safe arithmetic (`kz_surface_bytes`, `kz_surface_validate`); caps:
8192 px dim, 256 MiB surface (kernel keeps tighter 16 MiB/mode caps on top).

R1 scope: descriptor validation + transition rules as shared pure logic.
Stateful enforcement inside backends lands with the GHAL adapter / userspace
compositor (R2). `DisplayBuffer` and opaque `ghal_surface_t` unchanged.

## 3. Synchronization contract (`include/kz_sync.h`)

Stages are total-ordered and never conflated:
`SUBMITTED < RENDER_DONE < CONSUMER_DONE < RELEASED`.
A fence proves its stage and earlier ones only.

Mapping onto GHAL: submit accepted = `present_checked()==0` (failure keeps
damage dirty, retry later); virtio render-done = TRANSFER+FLUSH complete;
consumer-done = flush retired + watermark advanced; released =
`!fence_pending(f)`. Software completes all stages at submit
(`kz_stage_sync_submit()`, `kz_flush` returns `NULL` fence).

Rules: waits always bounded (`KZ_FENCE_WAIT_MAX_US` 0.5 s, no infinite
constant); timeout NEVER implies safe reuse (stay `IN_USE`, quarantine/poll);
never report async success on submit alone; never wait on GPU under an
unrelated global lock.

## 4. Native 2D API (`include/kz_render.h`, `libs/render/src/kz_render.c`)

Integer rects, scale+translate transform (`KZ_FIXED_ONE=1024`; rotate/skew
and paths deferred to R2 — no fake-success ops). Context borrows the target
(caller keeps pixels alive). `save/restore` pushes {transform,clip}, depth 8.
`clip_rect` intersects; empty clip = no-op OK. Paint = 0x00RRGGBB + 0..255
alpha src-over (XRGB dst treated opaque). Empty rect = `KZ_OK` no-op;
OOB writes clipped; scaling blit and format mismatch = `KZ_ENOSUP`;
same-target overlap safe (memmove order + reversed blend pass).
No per-primitive allocation. Text reuses `libs/text` via the existing
Painter path — no duplicate font code in R1.

## 5. Compatibility

- KWM ABI + syscall numbers unchanged; window policy stays in KWM.
- `compositor_flush_frame()` stays in kernel; single scanout owner; panic
  rendering independent of `kz_render`.
- Software renderer remains reference + fallback. GHAL append-only rule kept.
- Software `mode_set` (-1) and NULL cursor ops stay as specified rejections.
- VirtIO: cursor cap still unadvertised; host-resize `mode_changed=NULL`
  gap documented, not papered over (unsupported path must fail explicitly).

## 6. Tests

- `test-damage`: real display+KWM+compositor+software vs oracle (coalescing,
  superset, 5120-rect pressure, 16-slot, clip extremes, reentrancy,
  async model, opaque revoke). Was unregistered; R1 registered it
  (+`kernel/gfx/fb.c`, `color_blend.c` units it needs).
- `test-damage-driver`: real virtqueue+virtio transport vs RAM device
  (2-chain submit, fence watermark, out-of-order, lock contention, error
  propagation). Was unregistered; R1 registered it.
- `test-kz-surface`: desc validation, overflow, lifecycle, generation, stage
  order, fence release, wait budgets. New.
- `test-kz-render`: null/empty/OOB/clip/transform/alpha/repaint/border/blit/
  overlap/flush/wrap. New.
- Missing sources (no host target possible): `panic_test.c`,
  `textedit_test.cpp`, `virtqueue_test.c`, `virtio_gpu_cmd_test.c` — still
  absent; documented here, not silenced.
