# R4 Lifecycle — Metadata, Replacement, GHAL Integration

Prerequisite: R1/R2/R3 docs. Kernel compositor stays the sole scanout
owner. No triple buffering. No syscall renumbering.

## 1. Authoritative metadata (syscall 95, `kz_shm_stat_t` v1)

`kz_shm_stat` fills dims/stride/format/size/generation/owner/npages from
the live allocation, plus caller-relative rights (owner RW, published
consumer RO) and state (ACTIVE/PUBLISHED). Unpublished resources are
invisible to strangers (`KZ_EACCES`); dead handles report `KZ_ESTALE`,
never stale metadata. The out-pointer is range-validated before anything
is consumed. Creation already binds validated metadata (R3), so no
second create op was needed — the query exposes it authoritatively and
replaces producer-attested dims for new consumers.

## 2. Consumer references (syscalls 96/97)

`acquire` (published-only for non-owners) takes a counted ref;
`release` drops it and rejects non-holders (`KZ_EINVAL`, so an
unauthorized release is visible, not silent). Refs join mappings in the
reclamation gate: pages free only when destroyed AND unmapped AND
unreferenced. `kz_shm_task_exit` drops both. `consume` (kernel-internal)
borrows the page array under a transient ref for the SHM→adapter bridge.

## 3. Replacement (create-new, never mutate-in-place)

Create (new id+gen) → init → publish → consumers acquire the new handle
(metadata query shows the new dims) → destroy the old. Outstanding
holders keep working (pages retained, existing mappings valid) while
newcomers get `KZ_ESTALE`. Covered host-side (3 replace cycles, leak
balance) and in-QEMU (retire-old, old-stale, new-usable).

## 4. SHM→adapter→GHAL bridge (`kz_adapter_upload_shm`)

Uploads validated SHM content into a WRITABLE adapter surface, then marks
READY; present stays separate. Page-sized chunks via HHDM (handles any
stride, incl. rows wider than a page); rects carry surface position per
the GHAL dual-purpose convention; format must match (`KZ_ENOSUP`
otherwise); adapter-local state validated before the remote handle.
A failed upload/present never reports presentation: READY is set only on
full success, present failure keeps READY + `KZ_EBUSY`.

## 5. Stage separation (observable)

Published (stat=PUBLISHED) vs uploaded (adapter READY) vs submitted
(IN_USE, fence pending) vs released (poll after observed completion) —
each asserted distinctly in `test-kz-bridge`. Timeout never releases.

## 6. Paging hardening (R3 fix, R4 locked)

Leaf-PTE split helpers (`pte_frame`/`pte_flags` in `paging.h`,
host-tested) are now the only correct readers; SHM exclusion shares one
predicate (`kz_shm_va_excluded`). TLB rule enforced by design: no API
forcibly unmaps another AS (destroy retires lazily; self-unmap shoots
down locally; AS destroy flushes all). `protect` still cannot clear NX.

## 7. QEMU evidence (`_shm_probe.py`, virtio-vga)

`SHM DEMO PASS` (30+ checks, two processes): malformed/unpublished/wild
rejected, frame1 differential `DIRECT CK == SHADOW CK (859898437)`,
frame2-after-fork proves sharing (not a fork copy), metadata/acquire/
replacement/retire all OK, `FENCE LIVE` (present advanced, err 0), no
panic. Software path reports `FENCE SKIP` by design.

## 8. Compatibility

New syscalls only (95-97); KWM ABI, compositor, GHAL, backends, panic,
widget/desktop, Intel tree untouched (diff guard). Software fallback
intact.

## 9. Deferred (R5)

GPU-backed shared surfaces (needs import/export + backend ownership);
consumer-acknowledged replacement; `protect` NX clearing; forced cross-CPU
revocation (no mechanism, deliberately absent).
