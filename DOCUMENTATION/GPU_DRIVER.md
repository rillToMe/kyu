# GPU Driver — KyuzenOS Intel 2D Backend

All Intel code: `graphics/backend/intel_*`. 2D only — no 3D/GL/Vulkan/shaders.

## Hardware facts (AL-1, runtime-discovered + cross-checked)

- Physical GPU: `VEN_8086 DEV_468B REV_0C`, class `03/00` — Alder Lake-S
  UHD GT1, Gen12 (LKDDb `i915_pci.c`/`xe_pci.c` match + linux-hardware.org
  + host WMI; Intel driver INF groups it under Alder Lake).
- `0x468B` already falls in the driver's Gen12 range (`0x4680–0x469F`,
  from i915 `intel_device_info.c`) — no table change needed.
- BAR0 address/size print at boot (`intel_pci_diag`); values pending a
  physical boot (QEMU has no iGPU).
- Engine: exactly one copy engine, BCS0 (`adl_s_info.platform_engine_mask`
  = RCS0|BCS0|VECS0|VCS0|VCS2 — render/video ignored, never enabled).

## Hardware assumptions

- Intel iGPU as PCI VGA controller (vendor `0x8086`, class `0x03/0x00`).
  No hardcoded device ID; generation from ID ranges (`intel_regs.h`).
- MMIO = BAR0 (64/32-bit) + HHDM. Verified by DEVID readback (0/0xFFFF → fail).
- BAR validated (AL-2): memory type, known type bits, non-zero, page-aligned.
- Gen8–11: BCS legacy ring (frozen). Gen12: execlist submit (AL-5–AL-8).

## Init chain (`intel_init`, fail-closed, non-fatal at every stage)

PCI find (+rev/class capture) → gen detect → BAR0 (+validate + size
probe) → AL-1 PCI diag → bus-master enable → MMIO verify (+probe
PASS/unavailable diag) → AL-3 engine discovery → buffer alloc init →
GTT init → BCS init → status page → self-tests
(cmd/rect/fence/COPY/FILL/BLIT/surface) → Gen12 chain (VM → context →
batch → fence → ring → STORE submit → PPGTT → COPY/FILL/BLIT tests →
bench → SMP proof → robust → fault → AL-18 diag) → ready log. Any
failure before `g_intel_active` returns `-1` and GHAL tries the next
backend; every Gen12 stage fails closed independently.

## Backend selection (`ghal_init`)

`virtio-gpu → intel → software`. Diag after selection (`ghal_diag_dump`):
`backend: … / acceleration: enabled|disabled / engine: BCS|BCS0|none`.
Compositor uses `ghal_*` only; Intel exposes `acceleration_enabled()`
(BCS-live OR gen12-live) + `engine_name()` (`BCS0` when Gen12-live,
else `BCS`) via optional vtable ops. Final Gen12 block (AL-18):
`generation / submission: gen12|unavailable / acceleration /
fallback`, with `enabled` only when the boot STORE proved the engine.

## Supported ops (Gen12 dispatch when live, legacy BCS, else CPU)

| GHAL op | Gen12 live, private GPU surfaces | Else |
|---|---|---|
| fill_rect | XY_COLOR_BLT (7 DW, verified) + fence | legacy XY / CPU row loop / accessor |
| blit | XY_SRC_COPY 1:1 (10 DW, verified) + fence | legacy XY / CPU memcpy / accessor |
| upload | — (CPU transfer by definition) | row memcpy / per-page write |
| present | no-op (sync model: nothing outstanding) | no-op |
| scanout create | wraps fb (zero-copy) | kmalloc fallback |

Damage flow is untouched: per-rect upload/present from the compositor;
only validated, clipped rects reach the GPU. Window composition stays
CPU (KWM canvases are not GHAL surfaces — migrating them would be a
compositor redesign, out of scope; AL-14).

## Sync / SMP

Submit+fence-wait_sleep inside every HW op (CPU→GPU ordered by program +
submit barrier; GPU→CPU by fence; GPU→GPU in-order ring). Legacy: one
submission owner (`g_bcs_lock`). Gen12: THE single owner
(`gen12_submit_commit` — private staging, ring copy + mirror-sync +
image patch + ELSP under one irqsave lock; no wrap, TAIL strictly
grows). Buffer/GTT/seqno locks irqsave; ring-full → CPU fallback;
never sleep under a GPU lock. Lock order: buffer → GTT → fence →
submission (no cycles). `-smp 8` QEMU run BLOCKED (no QEMU in env);
ordering proven by the live-gated SMP self-test (code), not claimed.

## Failure matrix

No GPU / bad BAR / MMIO / GTT / BCS / status → fallback, boot continues.
Bad rect/buffer/command/fence → reject code, never crash (`robust`
self-test, 30+ cases, every boot; Gen12 `gen12_robust` + `gen12_fault`
suites extend it: timeouts bounded, alive-after-fault proven). All waits
bounded. GPU reset: N/A (legacy ring has no reset; none faked for Gen12).
Gen12 first-failure clears live (fail-fast, no per-frame stalls).

## Known limitations

- 256KB / 64-page buffer cap; 2MB GTT; no scaling; no HW cursor;
  no fb GTT mapping (scanout is CPU).
- Legacy XY builders + ring regs diverge from HW-proven values (see
  Architecture §9) — frozen pending Gen8–11 HW.
- Gen12 HW PASS (STORE/COPY/FILL/BLIT/bench numbers) pending a physical
  Alder Lake boot; QEMU validates nothing (no iGPU).
- XY blits may need aliasing-PPGTT work beyond the mirror if the HW run
  shows faults (flagged, not guessed); PAT index 0 assumed WB.
- Forcewake GT is held, never released (documented); GuC-absent assumed
  (nothing loads firmware, so the CS listens to ELSP).
