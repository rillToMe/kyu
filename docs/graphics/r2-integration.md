# R2 Integration — Adapter, Shadow Compositor, Differential Validation

Prerequisite: R1 at `b1598dd` (working tree; contracts in
`docs/graphics/r1-contracts.md`). Kernel compositor remains the sole active
display owner. No second scanout owner exists in R2.

## 1. Surface–GHAL adapter (`include/kz_adapter.h`, `kernel/gfx/kz_adapter.c`)

Handle = `(generation << 16) | (index+1)`, table 32 slots, quarantine 8.
Task context only (plain spinlock, no IRQ masking; never IRQ/panic path).

- Create validates the descriptor first, then allocates the backend
  surface; backend failure frees the slot (`KZ_ENOMEM`, nothing leaked).
- `acquire`: `CREATED|RELEASED -> WRITABLE`, else `KZ_ESTATE`.
- `upload`: `WRITABLE`-only, rect inside bounds (overflow-safe), then
  `ghal_surface_upload`; `READY` after. Rejected uploads keep `WRITABLE`.
- `present`: `READY`-only; `present_checked()<0` keeps `READY` + `KZ_EBUSY`
  (damage retained by caller); `0` records `present_fence()` and moves to
  `IN_USE`. No wait is taken under the adapter lock.
- `poll`: `IN_USE` + `!pending` -> `RELEASED`; pending -> `KZ_EBUSY`.
  A timeout never releases; quarantine is retained.
- `destroy`: any live state. In-flight + pending quarantines the backend
  surface (handle dies immediately: later use is `KZ_ESTALE`); the surface
  is freed when its fence retires. Double destroy -> `KZ_ESTALE`.
  Quarantine full -> `KZ_EBUSY` (poll, retry).
- `resize`: `IN_USE` + pending -> `KZ_EBUSY`. Otherwise replaces the
  backend surface (failure -> `KZ_ENOMEM`, old surface already freed, no
  leak), bumps generation, returns the new handle; the old handle is stale.
- CPU pixels are always caller-owned (borrowed, never freed here).
- Format values are `_Static_assert`-mirrored with GHAL; only XRGB/ARGB
  exist — anything else is `KZ_EINVAL`, never a silent fallback.
- Linked into the kernel (`kernel/CMakeLists.txt` `_gfx_sources`);
  symbols verified in `build/target/bin/myos.bin` (`nm`: all 8
  `kz_adapter_*` present in the rebuilt ISO).

## 2. Shadow compositor (`include/kz_shadow.h`, `libs/render/src/kz_shadow.c`)

In-process userspace prototype: snapshot (layers carry descriptors by
value, sorted stable by z) rendered to its own off-screen target via
`kz_render`. No scanout, no present, no window policy.

Blocker (evidence-backed, not faked): KyuzenOS has no cross-process
shared-surface mechanism in R2 (no shm/pixel IPC in the tree), so an
out-of-process compositor cannot be built safely yet. Process isolation
is R3 work; the prototype proves composition + damage semantics in-process.

Two-phase errors (validate all layers before writing; compose aborts with
`failed_index`, caller discards partial output), damage collapse to bounding
superset past 64 rects, invisible layers skipped, cross-format refused
(`KZ_ENOSUP`), identity transform enforced internally with entry state
restored. Cursor is an ordinary ARGB layer + damage rects (no HW needed).

R1 fix found by R2 work: `kz_blit` ignored per-pixel ARGB alpha (opaque
paint copied raw). Now `a_eff = (src_a * opacity + 127)/255`, same-target
overlap keeps memmove order with a reversed blend pass. Covered by new
`argb_checks` in `test-kz-render`.

## 3. Differential validation (`tests/host/unit/kz_shadow_test.c`)

Independent per-pixel oracle (no `kz_render` calls) + full-vs-incremental
equality. Byte-for-byte required and met: every op is exact integer with
documented rounding (`+127)/255`); no tolerance is needed and none is
claimed. Mismatches report coordinates + got/want (first 5).

Scenes: single opaque, overlapping opaque, translucent, off-screen,
fragmented damage (5 rects), repaint stability, destroy/replace, ARGB cursor
damage vs bounding rect, 70-rect overflow (union matches, outside bounding
box stays sentinel), 9 error paths, hand-computed spot
(128-white-over-black = `0x808080`).

## 4. Telemetry (host functional numbers, NOT frame latency)

- `test-damage` replay (host, 198 frames): avg 24 us, worst 181 us,
  mismatch 0. Measures the harness model, not QEMU scanout latency.
- New tests are functional (pass/fail + leak balance); wall time per binary
  ~0.02–0.03 s on this host. No QEMU frame timing claimed.
- Shadow stats per compose: damage rects, cleared/blended pixels,
  drawn/skipped layers, failing index.

## 5. Compatibility

Unchanged (verified via `git status`: not in the diff): `include/kwm_abi.h`,
`kernel/gfx/compositor.c`, `kernel/gfx/kwm.c`, `graphics/ghal.*`,
`graphics/backend/*`, `drivers/graphics/*`, `kernel/panic/*`,
`libs/gui/*`, `system/desktop/*`. Software `mode_set`/cursor rejections,
VirtIO cursor cap, and Intel freeze all preserved.

## 6. Verification

- Host: 27 passed / 3 disabled (`test-tls-bin`, `test-kyuzenfs-tree`,
  `test-libc-heap` — pre-existing, unrelated), ~2 s, clang 21.
- Target + ISO rebuilt with the pinned toolchain; adapter symbols in the
  kernel binary; QEMU `boot_check.py` PASS (COM1 marker).
- Deferred: GHAL async-backend adapter exercise beyond mocks (needs live
  VirtIO fence), out-of-process shadow (needs shared surfaces), triple
  buffering (no evidence presented).
