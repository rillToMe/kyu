# Kyuzen OS — Roadmap FIX

Daftar perbaikan terjadwal, urut prioritas. Satu file = satu perbaikan.
Patokan kerja: baca file FIX sebelum mulai; update status setelah selesai.

| File | Prioritas | Judul | Status |
|------|-----------|-------|--------|
| — | ✅ | Pseudo heap corruption (halaman ROM di PMM) | **DONE** — commit `7f89077`, post-mortem di `DOCUMENTATION/troubleshooting/2026-07-26-heap-corruption-bosd.md` |
| `FIX_001_task_exit_stack_uaf.md` | **P0** | `task_exit()` idle loop di stack task DEAD | **DONE** (2026-07-26) — per-CPU idle stack; `conc` 5/5 PASS di `-smp 4` |
| `FIX_002_vmm_user_pml4_race.md` | P1 | Global mutable `vmm_user_pml4` (race ELF load SMP) | **DONE** (2026-07-26) — target PML4 eksplisit; global dihapus |
| `FIX_003_as_cookie_smp.md` | P1 | `current_as_cookie` global (syscall 45) | **DONE** (2026-07-26) — cookie per-task + counter atomik |
| `FIX_004_kwm_window_ownership.md` | P1 | Ownership `kwm_windows` + stale canvas pointer | **DONE** (2026-07-26) — owner per window + validasi create |
| `FIX_005_user_apps_ring0.md` | P1 (epic) | App berjalan di Ring 0, pointer heap mentah lintas boundary | OPEN |
| `FIX_006_ring3_rsp0_frame_clobber.md` | **P0** | Frame preemption ring-3 tertimpa di per-CPU syscall stack (RSP0 global) — BOSD di app ke-4 | **DONE** (2026-07-27) — RSP0 per-task di `schedule_on_cpu`; 6 app konkuren stabil |
| `FIX_007_exec_from_spawned_task.md` | P2 | `sys_exec` dari task spawned (notepad→fileman dsb): app hasil exec mati senyap, task langsung DEAD | OPEN (2026-07-28) |
