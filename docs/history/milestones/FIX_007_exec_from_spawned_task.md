# FIX_007 — sys_exec dari task spawned: app hasil exec mati senyap

**Prioritas:** P2
**Status:** OPEN
**Tercatat:** 2026-07-28 (temuan QA Phase 5B)

## Gejala

`sys_exec()` dari task `TASK_KIND_SPAWNED` (app ring-3 hasil `sys_spawn`) tidak
menghasilkan app baru yang hidup. Window app tujuan tidak pernah muncul dan
task langsung DEAD — tanpa BOSD, tanpa pesan error.

Kasus teramati: notepad keluar (ESC / close) memanggil `sys_exec("fileman.elf")`
(`user_apps/notepad.c`, akhir `main`). Seharusnya task berlanjut sebagai
fileman. Kenyataannya task mati.

## Dampak

Seluruh rantai app-switching antar app GUI terpengaruh, karena pola yang sama
dipakai di kedua arah (`user_apps/fileman.c`):

- notepad → `sys_exec("fileman.elf")` saat keluar
- fileman → `sys_exec("notepad.elf")` (via `edit.tmp`) / `sys_exec("viewer.elf")`
  saat buka file

## Bukti (QA 5B, QEMU headless, 2026-07-28)

- `start notepad` ×2 → ESC ke keduanya → `sched` menunjukkan `tasks=1/3`:
  kedua slot spawned DEAD, tidak ada window tersisa.
- Jika exec berhasil, task seharusnya tetap RUNNING sebagai fileman dan window
  "File Manager" terlihat. Keduanya tidak terjadi (screenshot 2 detik setelah
  ESC tetap tanpa window fileman).
- `start fileman` langsung dari shell **bekerja normal** → masalah spesifik di
  jalur **exec dari task spawned**, bukan di app fileman.
- Perilaku sudah ada sejak 5A (bukan regresi 5B). Baru terlihat sekarang karena
  sebelum 5A tidak ada app GUI yang berjalan sebagai task spawned.

## Yang perlu diinvestigasi

1. **Kegagalan diam-diam sys_exec (paling mungkin).** Jika `elf_load_file`
   gagal (entry == 0), syscall 33 kembali tanpa mengubah RIP → `main` notepad
   selesai → `sys_exit` → task mati. Persis seperti observasi. Cari tahu di
   cabang mana gagalnya: file tidak ditemukan? create AS gagal? load gagal?
   Tidak ada pelaporan error di jalur ini — semua kegagalan senyap.
2. **Asumsi TASK_KIND_KERNEL di syscall 33.** Jalur exec ditulis era exec-chain
   shell: destroy windows → destroy AS → flush buffer → create AS → load ELF →
   set rip/rsp (`g_shell_return_rsp` sebagai fallback RSP). Periksa mana yang
   tidak valid untuk task spawned (mis. fallback RSP shell, semantik CR3).
3. **Fileman mati di awal startup.** Alternatif: exec berhasil tapi fileman
   langsung exit — mis. `gui_create_window` gagal (sys_kwm_create_window
   menolak? uheap gagal?), atau event sisa (ESC release) memicu exit.

## Pendekatan yang disarankan

- Serial log (pola `HEAP_WATCH_DEBUG` yang sudah ada) di: cabang kegagalan
  `elf_load_file` pada syscall 33, dan di awal `main` fileman (`sys_get_pid`
  print sudah ada pola di notepad).
- Bedakan tiga kemungkinan di atas dengan satu run berinstrumentasi.

## Cara repro

1. Build & boot QEMU (headless maupun tidak).
2. Login → `start notepad`.
3. ESC.
4. Ekspektasi: window File Manager muncul, task tetap RUNNING.
   Aktual: task DEAD (`sched`), tanpa window.
