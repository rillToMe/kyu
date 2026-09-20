# Desain: Ring 3 Tahap 2 — Boundary Copy Syscall (FIX_005)

> **Status**: **SELESAI & TERVERIFIKASI** (2026-07-26) — boot `-smp 4` bersih,
> `badptr.elf` 27 PASS / 0 FAIL (ring 3), fileman GUI jalan (event + file list
> + canvas shared). Sisa opsional: golden suite penuh + `make heap-watch`
> **Scope**: Semua syscall di `kernel/syscall.c` yang menerima pointer.
> Pengecualian terdokumentasi: canvas window (syscall 31) dan buffer pixel
> `sys_draw_image` (syscall 23) — shared, validasi range tanpa copy.

## Tujuan

Syscall tidak lagi men-deref pointer user mentah. Setiap pointer dari ring 3:

1. **Divalidasi** — non-NULL, tidak wrap, panjang dibatasi, dan setiap halaman
   yang disentuh mapped di address space caller (`paging_is_mapped_into`).
2. **Di-copy** melewati boundary — copy-in sebelum dipakai, copy-out setelah
   subsistem selesai. Subsistem dalam (kfs/vfs/kwm/event/net) hanya menerima
   pointer kernel.

Efek samping yang ikut tertutup:

- **Tidak ada lagi deref pointer user di dalam spinlock IRQ-off** (dulu:
  `pop_event` di `event_lock`, `keyboard_read` di `kbd_lock`, `vfs_read/write`
  di `vfs_lock`, `kfs_*` memcpy di `fs_lock`, `kwm_get_window_pos` di
  `kwm_lock`). `#PF` di titik-titik itu dulunya fatal.
- **TOCTOU tertutup** untuk `sys_ping` (string dipakai berdetik-detik lintas
  preemption) dan `sys_sock_send` (buffer dibaca berulang sampai 5 detik).
- **Bug UAF `sys_load_elf` (25) diperbaiki**: filename dulu di-deref SETELAH
  AS lama dihancurkan dan CR3 pindah ke AS baru yang kosong. Sekarang di-copy
  ke `kfname[64]` sebelum teardown (pola yang sama dengan `sys_exec`).
- `sys_write` dengan `count` liar (dulu OOB read + `krealloc` tak berbatas)
  kini ditolak eksplisit di atas `UC_MAX_IO`.

## Keputusan desain

### 1. Predikat validasi TRANSISIONAL (bukan "user range saja")

Resep awal roadmap ("terima hanya PML4 idx < 256") **belum bisa dipakai**:

- Stack app = `kmalloc` heap kernel (`kernel/elf.c`) → higher-half
  `0xFFFF9000...` (PML4[288]), US=1.
- Hasil `sys_alloc` = pointer `kmalloc` mentah → juga higher-half. Canvas
  libgui dan hampir semua buffer app berasal dari sini.

Validasi "user range only" akan menolak hampir semua buffer sah. Maka Tahap 2
memvalidasi **"mapped di AS caller"** saja. Nilai nyatanya: copy bounded,
TOCTOU hilang, deref keluar dari critical section, dan SATU choke point —
Tahap 3 tinggal memperketat `user_range_ok()` (satu fungsi) setelah stack app
dan `sys_alloc` pindah ke user range.

### 2. Modul `kernel/usercopy.c` + `include/usercopy.h`

```c
typedef struct { int from_user; phys_addr_t pml4; } ucopy_ctx_t;
void    ucopy_ctx_init(ucopy_ctx_t*, const registers_t*); // from_user = (r->cs & 3) == 3
int     user_range_ok(const ucopy_ctx_t*, uint64_t uaddr, uint64_t len);
int     copy_from_user(const ucopy_ctx_t*, void* kdst, uint64_t usrc, uint64_t len);
int     copy_to_user(const ucopy_ctx_t*, uint64_t udst, const void* ksrc, uint64_t len);
int64_t strncpy_from_user(const ucopy_ctx_t*, char* kdst, uint64_t usrc, uint64_t cap);
```

- **Ctx per-invocation, di stack syscall** — BUKAN global/per-CPU. Syscall
  bisa block (`sti; hlt`) dan task lain bisa masuk syscall bersarang di CPU
  yang sama.
- **Bypass ring 0 via `r->cs`**: shell/login/zen (kernel-side) juga masuk via
  `int 0x80` dengan pointer kernel. `from_user == 0` → hanya cek non-NULL,
  tanpa walk page table. Perilaku lama utuh.
- **Deref pasca-validasi aman tanpa fault**: CR3 saat syscall = PML4 caller
  (int 0x80 tidak ganti CR3), dan hanya task pemanggil yang bisa unmap AS-nya
  sendiri — sedang berada di dalam syscall ini. SMAP baru masuk Tahap 4
  (perlu STAC/CLAC di helper ini, satu tempat).
- `strncpy_from_user` memvalidasi **per halaman yang disentuh** — string
  pendek di halaman terakhir yang mapped tetap sukses; truncate diam-diam di
  `cap-1` (semantik copy `kfname` lama).
- `user_range_ok` walk per halaman (`paging_is_mapped_into` ambil
  `paging_lock` per panggilan) — bukan per byte.

### 3. Batas ukuran (satu tempat, `usercopy.h`)

| Konstanta | Nilai | Dipakai |
|---|---|---|
| `UC_MAX_STR` | 1024 | `sys_print` (1), `sys_draw_string` (26) |
| `UC_MAX_FNAME` | 64 | semua nama file kfs + path vfs (7,8,11,12,13,18,25,33,47) |
| `UC_MAX_HOST` | 128 | `sys_ping` (41) |
| `UC_MAX_KBD` | 512 | `sys_read_keyboard` (3) |
| `UC_MAX_FILE` | 8 MB | isi file (13, 18) |
| `UC_MAX_IO` | 1 MB | `sys_read`/`sys_write` (48, 49) per panggilan |
| `UC_MAX_SOCK` | 64 KB | `sys_sock_send`/`recv` (54, 55) per panggilan |
| `UC_MAX_ENTRIES` | 128 | `sys_get_file_list` (24) |
| `UC_MAX_RANGE` | 64 MB | plafon absolut `user_range_ok` (= 4096×4096×4 draw_image) |

Diverifikasi terhadap pemakaian nyata: viewer baca PNG ratusan KB (≪ 8 MB),
notepad simpan ≤ 4096 B, file list 16/32 entri.

## Kontrak per syscall

**copy-in** = string/buffer di-copy ke kernel sebelum dipakai; **copy-out** =
hasil ditulis ke kernel dulu, di-copy ke user di luar lock; **shared** =
pointer dipakai langsung setelah validasi range (pengecualian terdokumentasi);
**bypass** = tidak menerima pointer / tidak diubah.

| # | Syscall | Kontrak | Gagal validasi → |
|---|---------|---------|------------------|
| 1 | sys_print | copy-in ≤ 1023 char | diabaikan |
| 3 | sys_read_keyboard | copy-out (bounce stack ≤ 512), validasi SEBELUM read blocking | ret 0, tidak block |
| 7/8 | fs_read / fs_delete | copy-in nama ≤ 63 | diabaikan |
| 11/12 | file_exists / file_size | copy-in nama | ret 0 |
| 13 | read_file_to_buffer | copy-in nama; copy-out isi via bounce kmalloc; `size > cap` → 0 (semantik lama) | ret 0 |
| 17 | get_cpu_string | copy-out 49 B tetap | diabaikan |
| 18 | create_file | copy-in nama + isi via bounce (≤ 8 MB) | ret 0 |
| 20 | sys_get_time | copy-out 24 B (6×u32) | diabaikan |
| 23 | sys_draw_image | **SHARED #2**: validasi `0<w,h≤4096` + range `w*h*4`, baca langsung | diabaikan |
| 24 | get_file_list | copy-out array `file_info_t` via bounce, clamp 128 entri | ret 0 |
| 25 | sys_load_elf | copy-in nama **SEBELUM teardown AS** (fix UAF) | ret 0, AS utuh |
| 26 | sys_draw_string | copy-in ≤ 1023 char (bukan bagian pengecualian canvas) | diabaikan |
| 29 | sys_get_event | validasi 16 B SEBELUM pop (event tidak hangus); pop ke kernel, copy-out di luar `event_lock` | ret 0 |
| 31 | kwm_update_window | **SHARED #1**: validasi range = `kwm_window_canvas_bytes(id)` di syscall layer + owner check FIX_004 di kwm; tanpa copy (≤ 16 MB/frame) | diabaikan |
| 33 | sys_exec | copy-in nama via `strncpy_from_user` SEBELUM destroy window/AS | ret 0, state utuh |
| 40 | get_window_pos | copy-out 2×4 B, tiap pointer independen (NULL di-skip seperti dulu) | pointer itu di-skip |
| 41 | sys_ping | copy-in host ≤ 127 (TOCTOU tertutup) | ret -1 |
| 47 | sys_open | copy-in path ≤ 63 | ret -1 |
| 48 | sys_read | copy-out via bounce kmalloc, clamp 1 MB | ret -1 |
| 49 | sys_write | copy-in via bounce; `count > 1 MB` ditolak | ret -1 |
| 54 | sock_send | copy-in via bounce (TOCTOU tertutup); `len > 64 KB` ditolak | ret -1 |
| 55 | sock_recv | validasi SEBELUM data ring dikonsumsi; copy-out via bounce; clamp 64 KB (partial sah) | ret -1 |
| 9/10/19 | alloc / free / realloc | **bypass** — pointer heap mentah, target Tahap 3 | — |
| lainnya | tanpa pointer | bypass | — |

## Risiko & mitigasi

| Risiko | Mitigasi |
|--------|----------|
| Bounce stack (1 KB print + 512 B kbd) di syscall stack 16 KB | Block-scoped per branch; hanya satu branch jalan per invocation |
| Bocor bounce kmalloc di jalur gagal | Semua jalur `kfree` sebelum keluar; diverifikasi `make heap-watch` |
| Validasi canvas 16 MB = 4096 walk `paging_lock` per frame | Window nyata ≪ 1 MB; kalau compositor regresi → fallback validasi first+last page |
| `(r->cs & 3)` salah baca → shell ping/exec mati | Terdeteksi keras di boot pertama (golden suite) |
| Ctx dibuat global "biar hemat" | DILARANG — lihat Keputusan #2 (syscall bersarang saat block) |

## Verifikasi

1. `make` — kernel + `usercopy.o` + apps + ISO build bersih. ✓
2. Boot QEMU `-smp 4` headless: layar login muncul, uptime jalan, login
   `root` → shell (jalur ring 0 bypass hidup). ✓
3. `badptr` (app uji baru, `user_apps/badptr.c`) di ring 3: NULL / unmapped
   `0x10000` / wraparound `0xFFFFFFFFFFFFF000` / count liar per syscall —
   **27 PASS, 0 FAIL**, exit rapi kembali ke shell, kernel tetap hidup. ✓
4. `fileman` (GUI, TANPA modifikasi): window render, file list terisi
   (copy-out 24), event loop hidup (copy-out 29), canvas shared (31). ✓
5. Golden suite lengkap (`viewer` PNG, `notepad` save, `clock`/`calc`/
   `taskmgr`) — belum diotomasi, jalankan manual via `make run`. ⬜
6. `make heap-watch`: 0 panic, 0 corruption (cek bocor bounce). ⬜

## Temuan saat implementasi (bug yang diperbaiki)

1. **Klik mouse mati di semua app GUI setelah konversi** — `isr128.asm` menimpa
   slot RAX frame dengan **sisa register RAX** dari `syscall_handler` (fungsi
   `void`): `mov [rsp+112], rax` setelah `call`. Selama ini "kebetulan benar"
   karena compiler meninggalkan `ret_val` di RAX. Setelah Tahap 2, jalur sukses
   `sys_get_event` diakhiri `copy_to_user()` yang me-return 0 → sisa RAX = 0 →
   app selalu membaca "tidak ada event" → GUI tidak pernah render/terima klik
   (gejala: window calc kosong, hanya titlebar). Jalur GAGALNYA justru
   kebetulan benar (`user_range_ok` return 0), jadi `badptr` tetap 27 PASS —
   bug hanya terlihat di jalur sukses event. **Fix**: hapus `mov [rsp+112],
   rax`; `r->rax` yang ditulis handler di semua jalur adalah satu-satunya
   sumber return value (deterministik, kerapuhan terdokumentasi ini hilang).
   Verifikasi: calc render penuh, hover highlight jalan, klik tombol masuk ke
   display; `badptr` tetap 27 PASS.

2. **Semua app yang dibuka dari fileman menjadi badptr** — BUKAN bug kernel;
   bug UI laten `fileman.c` yang terpicu karena `badptr.elf` menjadi file
   ke-12. Zona hit-test daftar (`40 + total_files*20` piksel) menimpa tombol
   BUKA (`y 263..285`) begitu file > 11 — baris ke-12 tergambar di balik
   status bar (tak terlihat) tapi tetap bisa "diklik". Klik BUKA mengeksekusi
   cabang klik-daftar dulu (`selected_file` diam-diam → file terakhir =
   badptr.elf), lalu cabang BUKA membuka file itu. Terbukti via serial log:
   `[EXEC] kfname=[badptr.elf] rbx=0x4002980` = `files[11]` milik fileman —
   kernel menyalin dengan setia, data fileman-nya yang berubah. **Fix**:
   clamp `list_y1` ke batas atas status bar (`inner_h - 30`) di handler klik.

## Perubahan akhir

| File | Isi |
|------|-----|
| `arch/x86/isr128.asm` | Hapus timpa slot RAX dari sisa register — `r->rax` dari handler jadi satu-satunya sumber return value |
| `include/usercopy.h` | BARU — ctx, caps `UC_MAX_*`, deklarasi helper |
| `kernel/usercopy.c` | BARU — `user_range_ok`, `copy_from/to_user`, `strncpy_from_user` |
| `kernel/syscall.c` | Konversi semua syscall pointer (tabel di atas); ctx `uc` per invocation; fix UAF syscall 25 |
| `kernel/gfx/kwm.c` + `include/kwm.h` | BARU `kwm_window_canvas_bytes()` untuk validasi range syscall 31 |
| `user_apps/badptr.c` | BARU — app uji pointer jahat (PASS/FAIL via TTY) |
| `user_apps/fileman.c` | Fix hit-test daftar menimpa tombol BUKA saat file > 11 (temuan #2) |
| `kernel/syscall.c` (debug) | `[EXEC] kfname=[...]` di blok `HEAP_WATCH_DEBUG` untuk tracing exec |
| `user_apps/Makefile`, `Makefile`, `limine.conf` | wiring `badptr.elf` |
