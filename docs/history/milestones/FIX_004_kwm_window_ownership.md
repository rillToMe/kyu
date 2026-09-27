# FIX 004 — Ownership `kwm_windows` + stale canvas pointer

> **Prioritas**: P1
> **Status**: **DONE** (2026-07-26) — ownership per window + validasi create; regresi runtime bersih di `-smp 4`
> **Lokasi**: `kernel/` KWM (`kwm_create_window`, `kwm_destroy_window`, `kwm_destroy_all_windows`), call site di `kernel/syscall.c` (sys_exec/sys_exit)

## Masalah

`kwm_windows` global tanpa owner task/cookie:

1. `sys_exec`/`sys_exit` memanggil `kwm_destroy_all_windows()` → lifecycle satu
   task **menghancurkan window task lain**.
2. `kwm_destroy_window*()` tidak selalu meng-NULL-kan pointer canvas setelah
   `kfree` → compositor/render loop dapat menggambar ke canvas yang sudah bebas
   (stale writer ke heap — kandidat klasik "heap corruption" berikutnya).
3. `kwm_create_window()` menandai slot active **sebelum** `kmalloc` sukses, dan
   `width * height * 4` tidak divalidasi overflow.

## Rencana fix

1. Tambah `owner_task` (+ `cookie`) per window; `destroy_all` menjadi
   `kwm_destroy_windows_of(task_id)` — hanya milik pemanggil.
2. Setelah `kfree(canvas)`: `canvas = NULL`; compositor skip window tanpa canvas.
3. `create`: validasi `w/h` (batas maks + overflow `w*h*4`), tandai active hanya
   setelah alokasi sukses; rollback bersih saat gagal.
4. Render loop membaca state window di bawah aturan kepemilikan yang sama
   (tidak menyentuh window task yang sedang di-destroy di CPU lain).

## Verifikasi

- Dua app GUI bersamaan; exit salah satu → window app lain bertahan utuh.
- Exit app GUI → tidak ada write ke canvas bekas (pantau build `HEAP_WATCH_DEBUG`).
- `kwm_create_window` dengan `w*h` ekstrem → ditolak, slot tidak bocor active.

## Implementasi (selesai)

- `kwm_window_t` mendapat `owner_task` (−1 = kosong); window baru dimiliki
  `smp_current_task_id()` saat create.
- `kwm_destroy_windows_of(task_id)` menggantikan `destroy_all` di syscall
  exec/exit; `kwm_destroy_all_windows` hanya untuk path kernel/test.
- `kwm_free_slot`: `canvas = NULL` setelah `kfree` + putus `dragged_win_id`
  ke slot itu (drag state sempat bisa menggantung ke slot reused).
- Create: validasi `w/h` (maks 4096, total ≤16MB, hitung 64-bit anti-wrap),
  `kmalloc` DULU baru `active=1` — tidak ada zombie slot saat OOM.
- `sys_kwm_destroy_window` (32) & `kwm_update_window`: hanya pemilik.
- Catatan: compositor memang sudah membaca canvas di bawah `kwm_lock` —
  free juga di lock yang sama, jadi jendela UAF-nya tertutup struktural.
- Runtime (`HEAP_WATCH_DEBUG`, `-smp 4`): 0 panic, 4 canvas alloc, PNG sukses.
