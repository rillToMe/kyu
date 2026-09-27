# FIX 003 — `current_as_cookie` global (syscall 45 race di SMP)

> **Prioritas**: P1
> **Status**: **DONE** (2026-07-26) — cookie per-task + counter atomik; regresi runtime bersih di `-smp 4`
> **Lokasi**: `kernel/syscall.c` (syscall 45, `as_cookie_counter`, `current_as_cookie`)

## Masalah

Syscall 45 mengembalikan global `current_as_cookie` — nilai cookie milik task
yang **terakhir** exec di CPU mana pun, bukan milik pemanggil. Di SMP, dua task
dapat membaca cookie yang tertukar → identity/ownership check salah (dipakai
untuk asosiasi AS per task).

## Rencana fix

1. Syscall 45 membaca `syscall_current_task()->cookie` (atau
   `tasks[smp_current_task_id()].cookie`), bukan global.
2. Hapus `current_as_cookie`; pertahankan `as_cookie_counter` (monotonic,
   boleh global — hanya penanda unik, bukan identitas).
3. Audit pemakaian `cookie` lain (KWM, VFS) agar konsisten per-task.

## Verifikasi

- Dua task exec app berbeda lalu panggil syscall 45 bersamaan → masing-masing
  menerima cookie-nya sendiri, stabil lintas reschedule.

## Implementasi (selesai)

- Syscall 45 membaca `syscall_current_task()->cookie`; global
  `current_as_cookie` dihapus (sisa sebutan di komentar saja).
- `as_cookie_counter` dipertahankan sebagai generator monotonic, increment
  kini atomik (`__sync_fetch_and_add` via `as_cookie_next()`) — dua exec
  bersamaan tak mungkin berbagi nomor.
- Konsistensi: jalur exec 33 kini juga men-assign cookie (sebelumnya hanya
  jalur 25 — app dari shell chain ber-cookie 0).
- Audit: tidak ada pemakai cookie lain (KWM/VFS bersih).
- Runtime (`HEAP_WATCH_DEBUG`, `-smp 4`): 0 panic, PNG sukses.
