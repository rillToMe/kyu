# Desain: KyuzenFS — Dukungan Direktori (3 Fase)

> **Status**: SELESAI (2026-08-07) — build clean (`make boot_image.iso` 0 error);
> host self-check `kyuzenfs dir: OK` + `desktop manifest: OK`; verifikasi runtime
> manual oleh user (QEMU): desktop launcher dari `/apps`, `mkdir` + `ls` di
> Terminal, root hanya berisi folder `apps` + file user, `start <app>` tetap
> jalan.
> **Konteks**: KyuzenFS V3 sebelumnya **flat** — semua file di root directory
> (`current_fs.root_dir_sector`), tanpa konsep folder. Padahal `kfs_file_entry_t`
> sudah punya `flags` (`FLAG_FOLDER = 0x02`, tidak pernah dipakai) dan
> `start_sector`. Root directory hanyalah "chain sector berisi 16 entry @32
> byte, diakhiri `FAT_EOF`" — pola yang bisa dipakai ulang untuk folder mana
> pun: `start_sector` folder menunjuk ke chain miliknya sendiri, isinya entry
> lagi (file atau sub-folder). Karena entry lama semua `FLAG_FILE`, migrasi ini
> **additive**, bukan breaking change — image flat lama tetap valid.

## Arsitektur hasil

```
KyuzenFS = FAT-based, path absolut (tanpa cwd / "." / "..")
  "/"         → current_fs.root_dir_sector (chain sector berisi 16 entry @32B)
  "/apps"     → entry folder di root; start_sector → chain-nya sendiri
  "/apps/x.elf" → entry file di chain folder /apps

kfs_file_entry_t (32B): filename[23] | flags | start_sector | size_bytes
  FLAG_EMPTY=0x00 · FLAG_FILE=0x01 · FLAG_FOLDER=0x02
```

- **Folder = entry biasa.** Tidak ada on-disk "directory record" terpisah —
  folder hanya entry ber-flag `FLAG_FOLDER` yang `start_sector`-nya menunjuk ke
  chain directory sendiri. Seluruh primitif direktori (`find_entry_in`,
  `insert_dir_entry`, `resolve_dir_nolock`) memakai satu konsep chain ini.
- **Path absolut.** Komponen dipisah `/` (komponen kosong di-skip). Nama tiap
  komponen ≤22 char (limit `filename[23]`). Tidak ada rename/move, delete folder
  rekursif, relative path, atau current-directory — semua sengaja di luar scope.

## Perubahan per fase

### Fase 1 — Kernel core (folder sebagai entry biasa)

| File | Perubahan |
|---|---|
| `kernel/kyuzenfs.c` | `find_file_entry` (root-hardcoded) → `find_entry_in(dir_sector, name, out_entry, out_sec, out_idx)`. Ekstrak `insert_dir_entry(dir_sector, entry)` dari slot-finding + chain-extend `kfs_create_file`. Baru: `resolve_dir_nolock` + `split_last_nolock` + publik `kfs_resolve_dir`, `kfs_create_folder`, dan `kfs_get_file_list` path-aware. |
| `include/kyuzenfs.h` | Semua `kfs_*` menerima `char* path`; `kfs_get_file_list(path, buf, max)`; deklarasi `kfs_resolve_dir` + `kfs_create_folder`. |

`kfs_create_folder`: alokasi 1 sektor bebas sebagai chain folder baru
(`fat_table[sec]=FAT_EOF`, sektor di-`memset(FLAG_EMPTY)`), entry
`{name, FLAG_FOLDER, sec, 0}` dimasukkan via `insert_dir_entry` ke parent.
`kfs_delete_file` **menolak** `FLAG_FOLDER` — delete rekursif = fase terpisah
(sengaja, mencegah orphan chain).

### Fase 2 — Syscall layer (expose path ke user-space)

| File | Perubahan |
|---|---|
| `kernel/syscall.c` | Syscall **24** `sys_get_file_list` kini `(path, buffer, max)` — RBX=path, RCX=buffer, RDX=max; path user di-copy ke kernel via `strncpy_from_user` (pola boundary-copy yang sudah ada, bukan invent baru). Syscall **64** `sys_mkdir(path)` → `kfs_create_folder`. |
| `apps/userlib.c` + `include/userlib.h` | Wrapper `sys_get_file_list(path, buffer, max)` 3-arg + `sys_mkdir`. |
| Caller: `user_apps/desktop.c`, `fileman.c`, `terminal.c`, `viewer.c`, `badptr.c`, `test/desktop_manifest_test.c` | Sisip argumen `"/"`. |

**Keputusan backward-compat: opsi (b) rebuild-all.** Tidak ada kontrak ABI
publik (OS masih development, semua ELF di-rebuild bareng tiap boot) → syscall
24 diubah in-place tanpa alias. Syscall 64 dipilih karena 1–63 sudah terpakai
(59–63 = KWM).

### Fase 3 — Migrasi app ke `/apps/`

| File | Perubahan |
|---|---|
| `kernel/kernel.c` | Sebelum instalasi module Limine: pastikan `/apps` ada (`kfs_create_folder` sekali). Routing module: `.elf`/`.app` → `/apps/<nama>`, yang lain (`kyuzen.png`, `logo.png`) → root. |
| `kernel/elf.c` | `elf_load_file`: bare name (tanpa `/`) di-resolve ke `/apps/<nama>`; path absolut dipakai apa adanya. |
| `user_apps/desktop.c` | `discover_apps` scan `/apps` + baca manifest `/apps/<base>.app`. `e->elf` tetap bare name (spawn resolve via loader). |
| `apps/shell.c` + `user_apps/terminal.c` | Pre-check ELF (`sys_file_exists`) → `/apps/<nama>`. |
| `user_apps/badptr.c` | Cek `/apps/badptr.elf`. |
| `test/desktop_manifest_test.c` | Stub `sys_file_exists`/`sys_read_file_to_buffer` strip prefix `/apps/`. |

**Satu titik resolve ELF.** `sys_spawn` (57), `sys_exec` (33), `sys_load_elf`
(25), dan `kernel_userlib` semua memanggil `elf_load_file`. Resolve bare-name
ke `/apps` DI `elf_load_file` menutup seluruh jalur launching tanpa mengubah
setiap situs spawn/exec. `sys_file_exists`/`sys_read_file_to_buffer` **tidak**
diubah global — file user & temp (`view.tmp`, `edit.tmp`, `users.sys`,
`*.txt`, `*.png`) tetap di root.

**Penyimpangan dari roadmap:** roadmap menyarankan ubah `Makefile` +
`limine.conf`. Tidak diubah. `module_path` limine hanya lokasi load dari ISO,
dan kode instalasi (`kernel.c`) ekstrak basename dari `mod->path` apa pun
prefix-nya — jadi routing ke `/apps` harus terjadi di kode instalasi, bukan di
konfigurasi. Routing `kernel.c` adalah choke point yang sebenarnya.

## Verifikasi

- **Host self-check** (`test/kyuzenfs_dir_test.c`, pola `-iquote test -iquote
  include`, ATA+heap+spinlock di-mock di RAM): format → `mkdir /apps` →
  `create_file("/apps/test.elf")` → list `/apps` (1 file, `is_folder=0`) →
  list `/` (folder, `is_folder=1`) → duplikat ditolak per-folder → nested
  `/a/b/c.elf` → delete path-aware, folder tak bisa dihapus.
- **Host self-check** (`test/desktop_manifest_test.c`): `discover_apps` scan
  `/apps`, manifest `/apps/<base>.app` ter-parsing (hidden `desktop.elf`).
- **Manual QEMU** (user): desktop launcher dari `/apps`; `mkdir` + `ls` di
  Terminal; root hanya `apps` + file user; `start <app>` jalan; regresi
  Explorer/viewer/notepad.

## Batas sengaja (ponytail: ceilings)

- **Tidak ada rename/move file & folder**, **delete folder rekursif**
  (folder harus kosong/ditolak dulu) — fase terpisah.
- **Tidak ada relative path / current-directory** — semua path absolut.
- **Explorer tanpa navigasi folder** — `fileman.c` list root; klik folder
  `apps` tidak masuk. Fase 4 terpisah.
- **Terminal `ls` tanpa argumen path** — `ls /apps` belum bisa; cek isi
  `/apps` via Explorer/desktop.
- **Resolve `/apps` tanpa fallback root** — bare-name ELF selalu `→ /apps`;
  ELF user di root tidak akan ter-launch (tidak ada skenario saat ini).
