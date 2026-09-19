# GPU Commands — KyuzenOS Intel 2D Backend

One queue (BCS ring), synchronous. `intel_bcs.*`, `intel_cmd.*`,
`intel_fence.*`, `intel_irq.*`, `intel_rect.*`, `intel_test_*`.

## Ring (`intel_bcs.c`, Gen8–11 legacy)

32KB contiguous RAM, GTT-mapped at `0x0`. `RING_BASE` = GPU base,
`HEAD/TAIL` = byte offsets (8B-aligned, QW-padded with MI_NOOP),
`CTL` bit0 = enable (read-modify-write, readback-verified; rejected →
unavailable). Gen12 reports unavailable (needs execlists — not guessed).
Submit: free-space check (`used + new <= half`, else `-1` → CPU fallback),
wrap copy, barrier, TAIL write — all under `g_bcs_lock`.

## Encodings (`intel_regs.h`, `intel_cmd.c`)

MI (generation-stable): NOOP, BB_END, STORE_DWORD_IMM (len 4 + addr hi).
XY 2D (i915-derived): SRC_COPY 12 DW, COLOR_BLT 8 DW, 32bpp, clipping
disabled in builders. Staging is a 256-DW caller-owned buffer; builders
reject NULL/zero sizes. XY field order is HW-validated by the Phase 9–11
boot tests (PASS on Gen8–11 HW; SKIP + builder-check without BCS).

## Submit / fence / IRQ

`intel_gpu_submit()` appends STORE(status, seqno) + BB_END, submits, returns
a seqno fence (monotonic from 1; wrap clears status and restarts — safe:
all use is synchronous). No BCS → CPU-backed, immediately signaled.
`intel_fence_wait_sleep()` = `sti;hlt` loop (wake on any IRQ, IF-clear falls
back to pause) + `intel_irq_handler()` observation on completion.
Handler is lock-free (volatile snapshot + counter, no MMIO) — the same body
a future IDT vector will call. IMR unmask + IDT vector + EOI are deferred:
unmasking INTx with no vector would #GP on real HW.

## Validation gate (`intel_rect.h`)

Single gate before every op: clip-in-place, origin-OOB reject (covers
negative-as-uint32), 64-bit x+w overflow safety, 1:1 size contract, same-
buffer overlap reject (XY direction undefined → CPU `memmove` semantics),
NULL/unmapped buffer reject. GHAL fill/blit validate, then emit.

## Tests (boot, non-fatal, serial verdicts)

`cmd_selftest` (builders), `rect_selftest` (12 cases), `robust_selftest`
(30+ failure paths, runs on all machines), COPY/FILL/BLIT HW tests
(64x64 pattern → GPU → fence → CPU validate), `fence_test` (round-trip +
CPU semantics), `bench` (TSC cycles). Verdicts: `PASS / FAIL / SKIP`.
