# GPU Commands — KyuzenOS Intel 2D Backend

Two submission worlds (dispatched, never mixed): legacy BCS ring
(Gen8–11, frozen) and Gen12 execlists (new files). Common: `intel_rect.*`
gate, monotonic seqno fences, bounded waits, boot self-tests.

## Ring (`intel_bcs.c`, Gen8–11 legacy, frozen)

32KB contiguous RAM, GTT-mapped at `0x0`. `RING_BASE` = GPU base,
`HEAD/TAIL` = byte offsets (8B-aligned, QW-padded with MI_NOOP),
`CTL` bit0 = enable (read-modify-write, readback-verified; rejected →
unavailable). Gen12 reports unavailable (needs execlists — not guessed).
Submit: free-space check (`used + new <= half`, else `-1` → CPU fallback),
wrap copy, barrier, TAIL write — all under `g_bcs_lock`.

## Encodings

MI (generation-stable): NOOP, BB_END, STORE_DWORD_IMM (len 4 + addr hi;
Gen12 sets bit22 `MI_USE_GGTT` — the fence STORE targets GGTT with no
PPGTT roots programmed).

Legacy XY (Phase-8, frozen, DIVERGES from HW — see Architecture §9):
SRC_COPY 12 DW, COLOR_BLT 8 DW, depth 2.

Gen12 XY (`intel_gen12_batch.c`, authoritative — NOT legacy reuse):
opcodes `0x50/0x53` on Gen12 (Intel PRM OSRC TGL Vol 10); 10-DW COPY
and 7-DW FILL orders from IGT (`gem_latency`, `gem_streaming_writes`,
2025 blt patch) + i915 `emit_clear`; 32bpp depth = 3
(`BLT_DEPTH_32`); headers carry `WRITE_ALPHA|WRITE_RGB`; `BB_END`
plain (BIT(0) only inside the LRC restore image, never in batches).

## Submit / fence / IRQ

`intel_gpu_submit()` appends STORE(status, seqno) + BB_END, submits, returns
a seqno fence (monotonic from 1; wrap clears status and restarts — safe:
all use is synchronous). No BCS → CPU-backed, immediately signaled.
`intel_fence_wait_sleep()` = `sti;hlt` loop (wake on any IRQ, IF-clear falls
back to pause) + `intel_irq_handler()` observation on completion.
Handler is lock-free (volatile snapshot + counter, no MMIO) — the same body
a future IDT vector will call. IMR unmask + IDT vector + EOI are deferred:
unmasking INTx with no vector would #GP on real HW.

## Gen12 execlist submit (`intel_gen12_submit.c`)

Context image: 4 pages @`0x20000` — PPHWSP + register state from
`gen12_xcs_offsets` + `set_offsets` + `init_common_regs`
(`CTX_CONTROL=0x90009`, RING slots patched per submit à la
`lrc_update_regs`). Ring: 16KB @`0x25000` (i915 context default).
Descriptor: low = `LRCA|VALID|PRIV|LEGACY_32B|FORCE_RESTORE`
(`0x2010D`); high Gen11 format = SW_ID 1, instance 0 (BCS0),
counter++, class 3 (COPY). Submit = forcewake GT (`0xA188`/`0x130044`,
held) → `mfence` → ELSP `0x12230` HI-then-LO (`write_desc` order) →
fence wait. Assumes no GuC owns the engines; zero PDPs never
dereferenced (every CS touch is GGTT).

## Single owner (AL-15)

`gen12_submit_commit(dw, ndw)`: private staging → ring copy +
mirror-sync + image patch + ELSP under one irqsave lock. No wrap
(TAIL strictly grows; full → BLOCKED). Locks: buffer → GTT → fence
→ submission, no cycles; waits/allocs never under lock.

## Validation gate (`intel_rect.h`)

Single gate before every op: clip-in-place, origin-OOB reject (covers
negative-as-uint32), 64-bit x+w overflow safety, 1:1 size contract, same-
buffer overlap reject (XY direction undefined → CPU `memmove` semantics),
NULL/unmapped buffer reject. GHAL fill/blit validate, then emit.

## Tests (boot, non-fatal, serial verdicts)

Legacy: `cmd_selftest`, `rect_selftest` (12), `robust_selftest` (30+),
COPY/FILL/BLIT HW tests, `fence_test`, `bench`. Gen12: `vm_selftest`
(window + mirror), `batch_selftest` (absolute DW asserts + rejections),
fence selftest (encoding + monotonic + timeout), STORE submit test,
COPY/FILL/BLIT suites (5 cases each: aligned/unaligned/sub/page/multi,
chained, multi-op), `bench` (`submit_sync/fill/copy/blit_64`,
`cmd_gen`), SMP ordering proof (live-gated), `robust` (30+ Gen12 doors),
`fault` (bounded timeout + alive-after-fault), `ghal_check`
(public-API dispatch proof). Verdicts: `PASS / FAIL / SKIP / BLOCKED`.
