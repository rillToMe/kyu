# Plan: Tambah Dukungan Direktori ke KyuzenFS (3 Fase)

Referensi: `kernel/kyuzenfs.c`, `include/kyuzenfs.h`, syscall FS di `userlib.h` (`sys_get_file_list`, dkk — lihat percakapan sebelumnya).

## Temuan penting (kenapa ini lebih murah dari perkiraan awal)
`kfs_file_entry_t` **sudah** punya `flags` (termasuk `FLAG_FOLDER = 0x02`, saat ini tidak dipakai) dan `start_sector`. Root directory sendiri sebenarnya cuma "chain sector berisi 16 entry @32 byte, diakhiri `FAT_EOF`" — persis pola yang bisa dipakai ulang untuk folder mana pun: `start_sector` folder tinggal nunjuk ke chain sector miliknya sendiri, isinya entry lagi (file atau sub-folder).

Konsekuensi bagus: **ini additive, bukan breaking change.** Image lama yang flat tetap valid — semua entry lama `flags = FLAG_FILE`, tidak ada folder, tetap kebaca sama seperti sebelumnya oleh kode baru.

---

## Fase 1 — Kernel core: folder sebagai entry biasa

> **Status: SELESAI (2026-08-07)** — `find_entry_in` + `insert_dir_entry` +
> `kfs_resolve_dir` + `kfs_create_folder`; semua `kfs_*` path-aware;
> `kfs_get_file_list` set `is_folder`. Host test `test/kyuzenfs_dir_test.c` OK.

**Refactor wajib (hilangkan duplikasi & root-sentris):**
- `find_file_entry(filename, ...)` sekarang selalu mulai dari `current_fs.root_dir_sector`. Ubah jadi `find_entry_in(dir_sector, name, out_entry, out_sec, out_idx)` yang menerima sector direktori awal sebagai parameter — bukan hardcode root. `find_file_entry` lama jadi wrapper tipis: `find_entry_in(current_fs.root_dir_sector, ...)`.
- Blok "cari slot kosong di chain direktori, extend kalau penuh" di `kfs_create_file` (baris ~221–245) itu logic generik direktori, bukan spesifik file. Ekstrak jadi `insert_dir_entry(uint32_t dir_sector, kfs_file_entry_t* new_entry)` — dipakai baik oleh file maupun folder baru.

**Fungsi baru:**
- `int kfs_resolve_dir(char* path, uint32_t* out_dir_sector)` — jalan dari `root_dir_sector`, split `path` per `/` (skip komponen kosong akibat `//` atau leading/trailing slash), tiap komponen dicari via `find_entry_in` dengan filter `flags == FLAG_FOLDER`. Kalau salah satu komponen tidak ketemu / bukan folder → return 0. Path `"/"` atau `""` → langsung `root_dir_sector`, sukses.
  - **Scope sengaja dibatasi:** absolute path saja, tidak ada `.`/`..`, tidak ada concept "current directory". Itu cukup untuk kebutuhan sekarang (desktop scan `/apps`) dan bisa diperluas belakangan.
- `int kfs_create_folder(char* path)` — split path jadi (parent_dir, folder_name). Resolve parent via `kfs_resolve_dir`. Alokasi 1 sector kosong (`find_free_sector`) sebagai chain awal folder baru, `fat_table[sec] = FAT_EOF`, sector di-`memset(FLAG_EMPTY)` lalu ditulis. Bikin entry baru `{filename=folder_name, flags=FLAG_FOLDER, start_sector=sec_baru, size_bytes=0}`, panggil `insert_dir_entry(parent_dir_sector, &entry)`.

**Ubah signature fungsi existing supaya path-aware** (parent dir di-resolve dulu via `kfs_resolve_dir`, baru komponen terakhir dicari/ditulis di situ):
- `kfs_create_file(char* path, char* data, uint32_t size)`
- `kfs_exists(char* path)`
- `kfs_get_file_size(char* path)`
- `kfs_read_to_buffer(char* path, ...)`
- `kfs_delete_file(char* path)`
- `kfs_get_file_list(char* path, void* buffer, int max_entries)` — tambah param path; loop entry set `is_folder = (entries[i].flags == FLAG_FOLDER)` (sekarang di baris 431 selalu di-hardcode 0 — itu yang perlu dibenerin).

**Batasan yang tetap dijaga:**
- Nama tiap komponen path tetap maksimal 22 char (keterbatasan `filename[23]` per entry) — nested folder dalam tetap kena limit ini per level, bukan total path.
- Tidak ada rename/move folder, tidak ada delete folder rekursif — kalau dibutuhkan, jadi fase terpisah.

**Verifikasi Fase 1:** unit test host-side (pola sama seperti `test/desktop_manifest_test.c` sebelumnya) yang mock ATA + FAT in-memory: format → `kfs_create_folder("/apps")` → `kfs_create_file("/apps/test.elf", ...)` → `kfs_get_file_list("/apps", ...)` harus balikin 1 entry dengan `is_folder=0` → `kfs_get_file_list("/", ...)` harus balikin 1 entry `is_folder=1` (folder `apps`).

---

## Fase 2 — VFS & syscall layer: expose path ke user-space

> **Status: SELESAI (2026-08-07)** — syscall 24 `(path, buffer, max)` +
> syscall 64 `sys_mkdir`; opsi (b) rebuild-all (tak ada kontrak ABI). Wrapper
> userlib + semua caller di-update; command `mkdir` di Terminal (kendaraan
> verifikasi).

- Cek dispatch syscall existing (nomor 24 untuk `sys_get_file_list` — cari file dispatcher, kemungkinan `syscall.c` atau serupa, belum ada di file yang di-share, assistant perlu grep dulu). Ubah signature supaya nerima `path` (string user-space, perlu validasi/copy ke kernel space sebelum dipakai — cek pola validasi pointer syscall lain yang sudah ada di codebase, ikuti pola yang sama, jangan bikin baru).
- Tambah syscall baru `sys_mkdir(char* path)` → `kfs_create_folder`.
- **Backward compatibility:** kalau ada binary user-space lama yang manggil `sys_get_file_list` versi lama (tanpa path), putuskan salah satu:
  - (a) bump syscall number, syscall lama tetap ada sebagai alias `path="/"`, atau
  - (b) semua caller di-rebuild bareng (kemungkinan besar ini opsi realistis karena masih tahap OS development, bukan ABI stabil ke publik) — assistant tinggal update semua pemanggil di user_apps/ sekaligus.
  Assistant harus cek dulu apakah ada kontrak ABI/versioning yang harus dijaga sebelum milih (a) vs (b).
- Update `userlib.h`: `sys_get_file_list(char* path, file_info_t* buf, int max)`, `sys_mkdir(char* path)`.
- Update pemanggil existing yang perlu (`fileman.elf` kalau ada, `desktop.c` versi terbaru) supaya kompile dengan signature baru.

**Verifikasi Fase 2:** boot ke QEMU, dari terminal app panggil listing root (`/`) — harus tetap nampilin isi lama tanpa error. Test manual `sys_mkdir("/apps")` lalu list `/` — folder `apps` harus muncul dengan flag folder yang benar (kalau ada UI file manager yang render folder vs file beda icon, cek juga itu — kalau belum ada differensiasi visual, itu dicatat sebagai known gap, bukan blocker).

---

## Fase 3 — Migrasi: pindahin app ke `/apps/`, balikin desktop.c ke skema semula

> **Status: SELESAI (2026-08-07)** — `kernel.c` pastikan `/apps` + routing
> module `.elf/.app` → `/apps/`; `elf_load_file` resolve bare name → `/apps`;
> desktop scan `/apps`; pre-check shell/terminal/badptr di-update. **Deviasi:
> Makefile & limine.conf TIDAK diubah** — routing terjadi di kode instalasi
> (`module_path` limine hanya lokasi load dari ISO, basename selalu diekstrak
> kode kernel).

- Boot-time: kalau `/apps` belum ada (`kfs_resolve_dir("/apps", ...)` gagal), buat sekali via `kfs_create_folder` — taruh di titik init FS yang tepat (`kfs_init`, setelah deteksi magic valid) atau di loader module Limine yang sudah nulis file ke FS (cek yang mana yang lebih pas untuk urutan boot).
- Update Makefile + `limine.conf`: `module_path` untuk tiap `.elf` dan `.app` manifest jadi target `/apps/<nama>` alih-alih root.
- `desktop.c`: `discover_apps()` balik scan `/apps` (bukan `/` seperti kompromi sementara di iterasi sebelumnya), pakai `sys_get_file_list("/apps", ...)` dari Fase 2.
- File lain yang masih assume flat root (kalau ada — cek `fileman.elf` dan app lain yang manggil `kfs_*`/`sys_get_file_list`) ikut diaudit, supaya tidak ada yang diam-diam masih baca root sambil app-nya udah pindah ke `/apps`.

**Verifikasi Fase 3:** `make boot_image.iso`, boot di QEMU, desktop nampilin app dari `/apps` (bukan root), root (`/`) cuma isi folder `apps` (+ file user lain kalau ada). Regresi check: Explorer (`fileman.elf`) bisa navigate masuk ke folder `apps` dan lihat isinya — kalau UI Explorer belum support navigasi folder sama sekali, itu di luar scope 3 fase ini, dicatat sebagai fase 4 terpisah.

---

## Yang sengaja di luar scope (biar tetap dikit-per-milestone)
- Relative path / current-directory concept
- Rename & move (file maupun folder)
- Delete folder rekursif (folder isi harus kosong dulu sebelum bisa dihapus — atau ditolak dulu untuk sekarang)
- UI file manager: icon beda untuk folder, navigasi drill-down — cek existing `fileman.elf` dulu, mungkin perlu prompt terpisah kalau assistant nemuin itu butuh kerjaan signifikan.