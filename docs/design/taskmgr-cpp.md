# Task Manager C++ (`apps/taskmgr/`)

Refactor dari `apps/taskmgr.c` prosedural (satu file 219 baris, global mutable,
helper `itoa` manual) menjadi aplikasi C++ berlapis mengikuti arsitektur
`apps/settings/` — bukan arsitektur baru.

## Struktur

```text
apps/taskmgr/
├── platform.hpp         — satu-satunya gateway header C (extern "C" userlib/libgui)
├── format.hpp           — helper integer→std::string (header-only; std::to_string
│                          tidak ada di subset libc++ Kyuzen)
├── process_model.*      — snapshot proses + sort + kill (DATA + syscall, TANPA UI)
├── system_model.*       — snapshot CPU/RAM/disk/uptime (TANPA UI)
├── processes_page.*     — tabel PID/Name/UID/State + status (build sekali)
├── performance_page.*   — label + progressbar CPU/RAM/disk/uptime
├── memory_page.*        — Used/Total/Free + persen + batas ABI yang dinyatakan
├── task_manager.*       — TaskManagerApp: window, sidebar, tick 500 ms, aksi
└── main.cpp             — entry (objek app di dalam main, bukan global)
```

Aliran data satu arah: syscall → model → page → widget. Page tidak pernah
memanggil syscall; model tidak pernah menyentuh widget. Kill tetap
`sys_kill(pid)` — otorisasi di kernel (`proc_can_kill`: parent/root/self).

## Build

Bukan via `apps/Makefile` (C Pola lama) melainkan rule `TM_*` di root `Makefile`
(pola `ST_*` settings): kompilasi via SDK C++ wrapper
(`-fno-exceptions -fno-rtti -std=c++17`), link `USERAPP_LIB_OBJS` + glob objek
toolkit libui. Guard link: `_start` ada, 0 undefined symbol, tanpa runtime
exception/thread. ELF tetap `taskmgr.elf` (manifest/ikon/`start taskmgr`
tak berubah).

## API kernel

**Tidak ada API baru.** Dipakai ulang: `sys_proc_list`, `sys_kill`,
`sys_getpid`, `sys_total_ram`, `sys_used_ram`, `sys_get_cpu_usage`,
`sys_get_total_disk`, `sys_get_used_disk`, `sys_uptime`,
`sys_get_screen_size`, `ui_settings_load`.

## Keterbatasan jujur (bukan bug — batas ABI)

- Kolom CPU/Mem per-proses TIDAK ada: `proc_info_t` hanya pid/ppid/state/
  uid/gid/exit_code/name. Ditampilkan hanya yang ada.
- Rincian kategori memori (kernel/proses/page table/cache) TIDAK ada: kernel
  hanya mengekspos total+terpakai. Halaman Memory menyatakan ini eksplisit.
- `sys_kill` hanya 0/-1: "ditolak vs sudah keluar vs PID tak valid" tidak bisa
  dibedakan — status melaporkan `failed/denied` apa adanya. Hanya bunuh diri
  yang terdeteksi pasti (guard sisi app).
- Sort via shortcut `S` (PID→Name→State): libui table tidak punya klik-header.
- State `SLEEPING`/`BLOCKED` kernel ditampilkan `SLEEP`/`BLOCK` (kompatibel
  tampilan lama); `MAX_TASKS` = 16.

## Shortcut

`1/2/3` pindah page, `R` refresh, `E` end task, `S` ganti sort, `ESC`/X keluar.
(Pola settings: memungkinkan verifikasi headless via sendkey QEMU.)
