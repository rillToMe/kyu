# GPU Acceleration Benchmark — Phase 20

## Method

- Harness: `graphics/backend/intel_bench.c`, auto-run once from `ghal_init()`.
- Clock: raw TSC cycles (`rdtsc`), min+avg over N iters (min cuts IRQ noise).
  TSC rate is uncalibrated — compare numbers only within one boot.
- Present/present-fence: not timed separately (software zero-copy scanout:
  upload writes straight to the fb, present is a no-op).

## Measured baseline (QEMU, software backend, no BCS)

`qemu-system-x86_64 -cpu max -m 1G -smp 4`, clean boot, heap-corruption free.
Second boot shown (first boot ran ~2x slower under host load — TSC noise,
hence min-of-N reporting).

| Op | min (cycles) | avg (cycles) | n | Path |
|----|----|----|---|------|
| cpu_fill_64 (16KB) | 3,888 | 8,420 | 50 | cpu |
| cpu_fill_256 (256KB) | 68,630 | 98,688 | 20 | cpu |
| cpu_copy_64 (16KB) | 51,300 | 70,307 | 50 | cpu |
| cpu_copy_256 (256KB) | 781,290 | 1,143,144 | 20 | cpu |
| cmd_gen_copy+fill (2 cmds) | 414 | 6,943 | 200 | cpu |
| damage_1rect (1x 64x64) | 51,388 | 80,275 | 10 | cpu |
| damage_4rect (4x 64x64) | 207,828 | 223,408 | 10 | cpu |
| damage_16rect (16x 64x64) | 828,438 | 909,850 | 10 | cpu |
| damage_32rect (32x 64x64) | 1,662,590 | 1,773,614 | 10 | cpu |
| hw_submit_sync | — | — | — | skipped (no BCS) |
| hw_fill_64 | — | — | — | skipped (no BCS) |

Serial verdict on this machine: `[bench] hw_* skipped (no BCS)`.

## Component mapping

| Roadmap component | Status |
|---|---|
| damage calculation | not separately instrumented (compositor-owned) |
| command generation | measured: ~414 cycles min per copy+fill pair |
| CPU copy / blend | measured: table above |
| GPU submission / exec / sync | pending HW (harness ready: `hw_submit_sync`, `hw_fill_64`) |
| framebuffer presentation | zero-copy no-op, nothing to time |

## Workload coverage (honest)

Covered: small damage (1–32 rects of 64x64), fill/copy at 2 sizes.
Not covered: 1/4/16/32 full windows, opaque-window fast path, transparent
blend, full-screen damage, movement/destruction/scroll, >256KB surfaces
(GPU buffer cap is 64 pages). These need the KWM-level harness (Phase 21+).

## No optimization claims

GPU-faster is unproven. The numbers above are the CPU baseline to beat.
Reproduce on HW: boot a Gen8–11 machine, read `[bench]` lines from
`serial.log` — `hw_*` rows appear automatically when BCS is live.

## Bug found by benchmarking

`damage_32rect` overflowed its 256x256 dst (32 64px rects need 8 columns)
and panicked the heap check on the first measured boot. Fixed by sizing
the grid from the rect count (`intel_bench.c`). The heap canary did its job.

## Phase 21 — one measured optimization

The Phase 17–19 rework routed the Intel vtable CPU fallback through a
per-pixel accessor call. Measured on the same QEMU setup:

| Op | min (cycles) | avg | n |
|----|----|----|---|
| cpu_fill_64 (direct loop) | 12,148 | 135,106 | 50 |
| cpu_fill_call_64 (per-pixel call) | 457,994 | 960,173 | 50 |

~38x overhead on min → restored direct row loops for linear-RAM surfaces
in `intel_fill_rect`/`intel_blit` CPU fallback (`intel_init.c`), keeping
the accessor loop only for GPU-backed (non-linear) surfaces. No other
Phase-21 candidate was justified: cmd-gen is ~1K cycles (negligible),
damage upload is memcpy-bound (no C-level win), and every HW-side
optimization (batching, caching, async) stays speculative until HW
numbers exist — explicitly out per roadmap rule 4.

## Phase 22 — robustness verdict

`[robust] ROBUST: PASS` on the same boot: 30+ invalid-input cases across
allocator, GTT, BCS, builders, fences and rect gates all reject safely.
Init chain fails closed stage-by-stage (PCI → BAR0 → MMIO → GTT → BCS),
all waits are bounded, and a graphics failure can never take down the OS
(software fallback needs no GPU state at all). GPU reset is N/A — the
legacy BCS ring exposes no reset mechanism.

## AL-12 — Gen12 workloads (numbers pending HW)

Harness `graphics/backend/intel_gen12_bench.c`, same `[bench]` min/avg
TSC discipline, probe-gated (dead engine costs one timeout):

| Workload | Meaning | Status |
|---|---|---|
| `gen12_submit_sync` (N=10) | STORE-only round trip: submission overhead + fence floor | pending HW |
| `gen12_fill_64` (N=10) | fill 64 + submit + exec + sync | pending HW |
| `gen12_copy_64` (N=10) | copy 64 + submit + exec + sync | pending HW |
| `gen12_blit_64` (N=10) | blit 64 + submit + exec + sync | pending HW |
| `gen12_cmd_gen` (N=200) | batch-build CPU cost only | runs anywhere Intel init runs |
| `gen12 damage_*` | GPU damage upload | pending AL-14 (no GHAL damage path yet) |

No acceleration claims until these rows have numbers from the physical
Alder Lake boot. The CPU table above is the baseline to beat.

