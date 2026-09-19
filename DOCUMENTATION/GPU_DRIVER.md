# GPU Driver — KyuzenOS Intel 2D Backend

All Intel code: `graphics/backend/intel_*`. 2D only — no 3D/GL/Vulkan/shaders.

## Hardware assumptions

- Intel iGPU as PCI VGA controller (vendor `0x8086`, class `0x03/0x00`).
  No hardcoded device ID; generation from ID ranges (`intel_regs.h`).
- MMIO = BAR0 (64/32-bit) + HHDM. Verified by DEVID readback (0/0xFFFF → fail).
- Target engine: BCS legacy ring (Gen8–11). Gen12/Alder Lake detected but
  reported unavailable (execlists) — CPU fallback continues.

## Init chain (`intel_init`, fail-closed, non-fatal at every stage)

PCI find → gen detect → BAR0 → bus-master enable → MMIO verify →
buffer alloc init → GTT init → BCS init → status page → self-tests
(cmd/rect/fence/COPY/FILL/BLIT/surface) → ready log. Any failure before
`g_intel_active` returns `-1` and GHAL tries the next backend.

## Backend selection (`ghal_init`)

`virtio-gpu → intel → software`. Diag after selection (`ghal_diag_dump`):
`backend: … / acceleration: enabled|disabled / engine: BCS|none`.
Compositor uses `ghal_*` only; Intel exposes `acceleration_enabled()`
(BCS-live) + `engine_name()` ("BCS") via optional vtable ops.

## Supported ops

| GHAL op | BCS live, private GPU surfaces | Else |
|---|---|---|
| fill_rect | XY_COLOR_BLT + fence | CPU row loop (linear) / accessor |
| blit | XY_SRC_COPY 1:1 + fence | CPU memcpy/accessor |
| upload | — (CPU transfer by definition) | row memcpy / per-page write |
| present | no-op (sync model: nothing outstanding) | no-op |
| scanout create | wraps fb (zero-copy) | kmalloc fallback |

Damage flow is untouched: per-rect upload/present from the compositor;
only validated, clipped rects reach the GPU.

## Sync / SMP

Submit+fence-wait_sleep inside every HW op (CPU→GPU ordered by program +
submit barrier; GPU→CPU by fence; GPU→GPU in-order ring). One submission
owner (`g_bcs_lock`); buffer/GTT/seqno locks irqsave; ring-full → CPU
fallback; never sleep under a GPU lock. Verified on `-smp 8` QEMU boots.

## Failure matrix

No GPU / bad BAR / MMIO / GTT / BCS / status → fallback, boot continues.
Bad rect/buffer/command/fence → reject code, never crash (`robust`
self-test, 30+ cases, every boot). All waits bounded. GPU reset: N/A
(legacy ring has no reset). Gen12 HW submit: blocked on execlists.

## Known limitations

- 256KB / 64-page buffer cap; 2MB GTT; no scaling; no HW cursor;
  no fb GTT mapping (scanout is CPU); XY field order proven only by the
  boot HW tests; HW numbers only on Gen8–11 physical machines.
