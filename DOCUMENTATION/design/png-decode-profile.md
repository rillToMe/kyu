# Profiling: kenapa membuka file PNG terasa berat

> **Status: LAPORAN ANALISIS — tidak ada kode yang diubah.**
> Semua instrumentasi yang dipakai untuk mengukur sudah di-revert; `git status`
> dan `git diff` bersih, `HEAD` masih `18389eb`, dan `disk.img` tidak tersentuh.
> File ini satu-satunya artefak yang tersisa.
>
> Tanggal: 19 Sep 2026 · Basis kode: `libs/widget/include/primitives/image.hpp`
> + `apps/png.c` + `core/painter.hpp` (kode yang sama dengan `apps/libui.cpp`
> baris 1287–1333 sebelum pemecahan).

---

## 1. Jawaban singkat

**Masalahnya bukan render, dan bukan alokasi, dan bukan `Painter::image()`.**
Untuk satu kali buka file, waktunya habis di **I/O baca file (42–90%)** dan
**kerja stb_image (9–36%)**. Kolom "alloc" dan "convert" masing-masing hanya
0.1–4%.

| Sample (pertama kali dibuka, cache FS dingin) | Baca file | Decode stb | Alokasi | Convert | Total | I/O | Decode |
|---|---|---|---|---|---|---|---|
| `small512.png` 46 KB, 512×512 | 32.4 ms | 26.6 ms | 2.3 ms | 3.1 ms | **76.8 ms** | 42% | 35% |
| `kyuzen.png` 179 KB, 1024×1024 | 118.7 ms | 78.5 ms | 8.8 ms | 12.5 ms | **221.0 ms** | 54% | 36% |
| `logo.png` 1.36 MB, 1024×1024 | 848.6 ms | 319.7 ms | 13.3 ms | 14.0 ms | **1197.3 ms** | 71% | 27% |
| `big6mb.png` 6.88 MB, 1920×1200 | 12 082.8 ms | 1223.7 ms | 34.5 ms | 28.9 ms | **13 375.1 ms** | 90% | 9% |

Angka bare-metal (QEMU, bukan host). Rincian + peringatan variansi di §3 dan §6.

**Proporsi kasar untuk file "sedang" (1–7 MB): ~70–90% I/O, ~10–30% decode,
<1% alokasi, <1% convert.** Untuk file besar, I/O hampir semuanya.

Dua penjelasan akarnya:

1. **Baca file ~0.6–1.6 MB/s** karena jalur baca KyuzenFS menembak **satu
   perintah ATA PIO per 512 byte sektor** (`drivers/ata.c:44`), 256× `inw` per
   sektor, tanpa DMA dan tanpa read-ahead. Bukan karena PNG-nya besar.
2. **stb_image (`STBI_NO_SIMD`, `-mno-sse`) harus mengembang-buka zlib +
   unfilter + RGBA**: 78 ns/pixel (1024²) sampai 532 ns/pixel (1920×1200).

Yang **tidak** jadi masalah: `Painter::image()` = 16.6 ns/pixel (5.85 ms untuk
351 649 pixel, satu frame) — untuk window viewer 446×374 itu ~2.8 ms/frame.

---

## 2. Metodologi (dan apa yang TIDAK bisa diukur)

### Yang diukur, di mana

Semua angka di laporan ini **diukur di bare metal (QEMU, kernel + ring-3 asli)**,
bukan disimulasikan di host. Caranya:

- **Instrumentasi sementara** di `apps/png.c` (pembungkus waktu per fase +
  penghitung `sys_alloc`/`sys_free`/`sys_realloc`) dan di
  `core/painter.hpp` (akumulator biaya blit). Ditambah app ring-3 sementara
  (`user_apps/_pngprof.c`, `void main(int argc, char* argv[])`) yang:
  kalibrasi TSC lewat `sys_uptime()`+`sys_sleep(300)`, jalankan microbenchmark
  alokator, `png_decode()` tiap file **dua kali** (pass 1 = cache FS dingin,
  pass 2 = langsung sesudahnya), lalu frame pertama lewat `ui_window_run()` +
  `ui_tick_cb`, dan menulis hasil ke `prof.txt` di KyuzenFS.
- Timing pakai `rdtsc` (CR4.TSD tidak diset → aman di ring 3), dikalibrasi:
  **2 607 724 cycle/ms ≈ 2.61 GHz** (QEMU `-cpu max`, 4 vCPU, 1 GB).
- Sample disuntikkan ke **salinan** `disk.img` (`test_disk_prof.img`) memakai
  kode KyuzenFS V4 yang asli di host (pola `test/kyuzenfs_xcheck.c`);
  `disk.img` asli tidak pernah ditulis. Hasil `prof.txt` diambil kembali dari
  image dengan cara yang sama.
- QEMU headless (`-display none -vga std`), monitor TCP untuk login (`root`,
  `1`) + `sendkey`, pola `test/_ui_probe.py`.

Semua file di atas (instrumentasi, app, alat host, runner, sample, salinan
image) sudah **dihapus/di-revert** (§7).

### Batasan yang harus dibaca sebelum memakai angka ini

1. **QEMU ≠ hardware asli, terutama untuk I/O.** Setiap `inb`/`inw`/`outb` di
   jalur ATA PIO adalah VM exit (~1–2 µs di QEMU). Jadi throughput baca
   absolut 0.6–1.6 MB/s itu **batas bawah QEMU**. Di hardware asli PIO
   sektor-per-sektor jauh lebih cepat (mungkin 10–30 MB/s), tapi
   **strukturnya tetap sama**: satu perintah disk per 512 B, tanpa DMA,
   tanpa read-ahead. Proporsi "I/O mendominasi" tetap, angkanya tidak.
2. **Variansi besar.** File yang sama, dua pass berurutan, bisa beda 80%
   (`logo.png`: 848 ms lalu 1504 ms). Penyebab: I/O host + emulasi + fsync
   periodik kernel. Perlakukan angka baca sebagai **±50%**.
3. **Viewer asli tidak dijalankan lewat UI** (butuh klik mouse). Yang diukur
   adalah jalur kode yang sama persis (`png_decode` + widget `Image` +
   `Window::render` + `Painter::image` + flush), lewat app profiling. Jadi
   angka decode/blit valid; "waktu buka di viewer" = angka itu + sedikit
   overhead shell/list.
4. Baris fase pada decode yang **gagal** (§4, `big2400.png`) tidak bermakna:
   `png_decode` keluar lebih awal, jadi fase setelah `read` masih nilai lama.
5. Kuantisasi: `sys_uptime()` dipakai hanya untuk kalibrasi; semua fase
   dilaporkan dari `rdtsc` (resolusi µs).

---

## 3. Tabel utama: per sample, per tahap

Satuan **mikrodetik**, `pass 1` = cache FS dingin, `pass 2` = langsung sesudah.

| Sample | fsize | file_size | alloc_raw | **read** | **stbi** | alloc_out | convert | free_raw | free_px | **total** |
|---|---|---|---|---|---|---|---|---|---|---|
| `small512.png` 46 077 B (512×512 RGBA) p1 | 46 077 | 11 818 | 90 | **32 372** | **26 593** | 2 230 | 3 051 | 27 | 657 | **76 845** |
| p2 | | 82 | 78 | 500 | 25 454 | 2 062 | 2 901 | 24 | 328 | 31 436 |
| `kyuzen.png` 182 832 B (1024×1024 pal4) p1 | 182 832 | 322 | 639 | **118 712** | **78 497** | 8 140 | 12 484 | 144 | 1 600 | **220 975** |
| p2 | | 138 | 435 | 2 039 | 74 016 | 7 483 | 9 074 | 137 | 894 | 94 221 |
| `logo.png` 1 394 920 B (1024×1024 RGBA) p1 | 1 394 920 | 120 | 2 879 | **848 626** | **319 721** | 10 381 | 14 014 | 351 | 1 201 | **1 197 300** |
| p2 | | 11 175 | 3 286 | 1 503 711 | 328 539 | 7 444 | 11 524 | 270 | 979 | 1 866 934 |
| `big6mb.png` 7 209 128 B (1920×1200 RGBA) p1 | 7 209 128 | 98 | 12 837 | **12 082 807** | **1 223 707** | 21 662 | 28 893 | 2 132 | 2 995 | **13 375 137** |
| p2 | | 3 075 | 14 658 | 7 715 345 | 1 203 166 | 19 956 | 21 796 | 2 129 | 2 722 | 8 982 852 |
| `big2400.png` 12 761 317 B (2400×1600 RGBA) p1 | 12 761 317 | 7 075 | 25 306 | 165 | 91 | — | — | — | — | **35 619 (GAGAL)** |

- `file_size` = syscall 12 (`kfs_get_file_size`). **Perhatikan `small512.png` pass 1:
  11.8 ms hanya untuk menanyakan ukuran file** — itu satu kali *path walk* dingin
  yang harus membaca inode dari disk. Setelah hangat: 82 µs.
- `read` = syscall 13 (baca file → buffer). Dingin: ~1.4–1.6 MB/s untuk file
  kecil/sedang, **0.57 MB/s** untuk `big6mb.png`.
- `stbi` = `stbi_load_from_memory` (inflate + unfilter + RGBA). Ini **termasuk**
  alokasi internal stb (lihat §5).
- `convert` = loop `apps/png.c:43` (RGBA bytes → XRGB8888, byte per byte).
  ~3.1 ms/MB, konsisten di semua sample.
- `alloc_*` = `sys_alloc` milik `png.c` sendiri (buffer file + buffer output).

### Alokasi per decode (jumlah panggilan + volume)

| Sample | `alloc_n` | `free_n` | `realloc_n` | Σ byte diminta¹ | buffer out |
|---|---|---|---|---|---|
| `small512.png` | 5 | 5 | 2 | 3 294 717 | 1 048 576 |
| `kyuzen.png` | 6 | 6 | 1 | 10 329 073 | 4 194 304 |
| `logo.png` | 5 | 5 | 6 | 18 115 816 | 4 194 304 |
| `big6mb.png` | 5 | 5 | 1 | 42 082 759 | 9 216 000 |

¹ Termasuk `new_size` tiap `realloc` (jadi ini volume kumulatif, bukan puncak).
Sisanya (`Σ - raw - out`) adalah buffer internal stb/zlib.

**Kesimpulan angka ini:** jumlah alokasi sedikit (5–6 + realloc), bukan
"ratusan buffer kecil" seperti dugaan awal. Yang mahal per byte adalah
*halaman yang di-map + di-zero per alokasi*, bukan jumlah panggilannya (§5).

### 3.1 Tabel ringkas (kolom persis seperti yang diminta)

| Sample | Waktu baca file | Waktu decode (stbi) | Jumlah `sys_alloc` | Waktu draw pertama | Total |
|---|---|---|---|---|---|
| `small512.png` 46 KB, 512×512 (RGBA) | 32.4 ms | 26.6 ms | 5 (+2 realloc) | tidak diukur per-sample ¹ | **76.8 ms** |
| `kyuzen.png` 179 KB, 1024×1024 (palet 4-bit) | 118.7 ms | 78.5 ms | 6 (+1 realloc) | **5.85 ms** blit (frame pertama 28.2 ms) | **221.0 ms** |
| `logo.png` 1.36 MB, 1024×1024 (RGBA) | 848.6 ms | 319.7 ms | 5 (+6 realloc) | tidak diukur per-sample ¹ | **1197.3 ms** |
| `big6mb.png` 6.88 MB, 1920×1200 (RGBA) | 12 082.8 ms | 1223.7 ms | 5 (+1 realloc) | tidak diukur per-sample ¹ | **13 375.1 ms** |
| `big2400.png` 12.76 MB, 2400×1600 (RGBA) | 165 µs — **DITOLAK** (file > 8 MB, §4) | 91 µs (gagal) | 1 | — | **35.6 ms (gagal)** |

Semua angka = pass 1 (cache FS dingin), mikrodetik diubah ke ms.

¹ **Draw pertama hanya diukur untuk `kyuzen.png`** — run profiling ini membuat
satu window + satu render (`draw_report()` dipanggil untuk `argv[1]` saja).
Yang bisa dipakai untuk sample lain adalah konstanta **terukur** 16.6 ns per
pixel *tampil* (§5.6) — angka per-sample bisa dihitung dari situ, tapi itu
**turunan, bukan hasil ukur**, jadi tidak saya tulis sebagai data di tabel.
Kalau kolom ini perlu angka ukur untuk semua sample, harness profiling harus
dipasang ulang (semua artefak sementaranya di §9) dan dijalankan sekali lagi
(~1 run QEMU); belum saya lakukan karena tidak mengubah kesimpulan:
bottleneck-nya di baca file, bukan blit.

---

## 4. Temuan paling penting: PNG > 8 MB gagal total (dan diam-diam)

`big2400.png` (12.76 MB, 2400×1600) → `ok=0`, `read_rc=0`, selesai dalam 35 ms.

Rantainya:

- `kernel/syscall.c:319` → `else if (fsize <= cap && fsize <= UC_MAX_FILE && ...)`
- `include/usercopy.h:24` → `#define UC_MAX_FILE (8u * 1024u * 1024u)`

Jadi syscall 13 **menolak** file > 8 MB dan mengembalikan 0 — tapi:

- `apps/png.c:34` → `sys_read_file_to_buffer((char*)filename, (char*)raw, fsize);`
  **nilai kembaliannya dibuang.** Tidak ada `if (!rc) return 0;`.
- Akibatnya `stbi_load_from_memory` diberi buffer yang isinya nol
  (`uheap_alloc` sudah men-zero-kan halaman), gagal, `png_decode` mengembalikan 0,
  dan `Image::draw` (`image.hpp`) jatuh ke cabang "tidak ada gambar" → kotak abu-abu.
- **User tidak dapat pesan apa pun.** `sys_file_size` (12.7 MB) tetap sukses, jadi
  app tidak tahu bahwa bacaannya ditolak.

Ini batas keras: **PNG terbesar yang bisa dibuka OS ini adalah 8 MB**, dan
kegagalannya senyap. (Ditemukan saat profiling, **tidak diperbaiki** di sini.)

---

## 5. Titik terberat di kode (dengan nomor baris)

### 5.0 Titik terberat di `apps/png.c` itu sendiri

`png_decode` hanya punya 6 baris yang benar-benar bekerja. Porsi waktunya
(pass 1, dari tabel §3):

| Baris `apps/png.c` | Apa yang terjadi di situ | Porsi waktu |
|---|---|---|
| `:29` `sys_file_size()` | syscall 12 → `kfs_get_file_size` → *path walk* + baca inode. Dingin: **11.8 ms** untuk file 46 KB; hangat 82 µs | 0.1–15% |
| `:32` `sys_alloc(fsize)` | buffer isi file; map + zero halaman (≈2.4 ms/MB) | 0.1–0.2% |
| **`:34` `sys_read_file_to_buffer()`** | **syscall 13 = 4× path walk + `kmalloc(fsize)` buffer bounce + baca disk sektor-per-sektor + `copy_to_user(fsize)`. Nilai baliknya dibuang** | **42–90%** ← terberat |
| `:37` `stbi_load_from_memory()` | inflate + unfilter + RGBA; **termasuk alokasi internal stb** (`STBI_MALLOC` = `sys_alloc`, `apps/png.c:20`) | 9–36% |
| `:41` `sys_alloc(iw * ih * 4)` | buffer output **kedua** seukuran pixel RGBA | 0.2–4% |
| `:43` loop konversi | RGBA→XRGB8888 byte-per-byte, 1 pass | 0.2–6% |
| `:54` `png_free()` | `sys_free` buffer output (dipanggil caller) | — |

Jadi di dalam `png.c` sendiri, yang terberat **bukan** loop konversi dan
**bukan** alokasi: itu **baris 34**, satu panggilan baca file yang di dalamnya
(kernel) menghabiskan 42–90% waktu. Rincian jalur kernel-nya di §5.2, dan
penjelasan kenapa baca disk lambat di §5.1.

### 5.1 I/O: satu perintah ATA per 512 byte (`drivers/ata.c`)

```
drivers/ata.c:44   void ata_read_sector(uint32_t lba, uint8_t* buffer) {
drivers/ata.c:46       ata_wait_bsy();                       // polling inb
drivers/ata.c:49       outb(ATA_DRIVE_PORT, 0xE0 | ...);      // command per sektor
drivers/ata.c:50       ata_delay_400ns();                     // 4x inb (drivers/ata.c:17)
drivers/ata.c:53       outb(ATA_SECTOR_COUNT_PORT, 1);        // <- SELALU 1 sektor
drivers/ata.c:61       outb(ATA_COMMAND_PORT, 0x20);          // READ SECTORS
drivers/ata.c:64       ata_delay_400ns();                     // 4x inb lagi
drivers/ata.c:67       if (ata_wait_drq() != 0) { ... }       // polling inb
drivers/ata.c:75       for (int i = 0; i < 256; i++)          // 256x inw per sektor
drivers/ata.c:76           ptr[i] = inw(ATA_DATA_PORT);
                       }
drivers/ata.c:166  int ata_read_block4k(uint64_t block_num, void *buf) {
drivers/ata.c:171      ata_read_sector(lba0 + (uint32_t)s, p + (s * 512));  // 8x
                       }
```

Setiap 4 KB blok = 8 perintah disk; setiap perintah = BSY-poll + 2×400 ns delay +
DRQ-poll + **256 port read 16-bit**. Tidak ada `READ MULTIPLE`, tidak ada DMA,
tidak ada prefetch. Perhitungan dari data terukur:

- `big6mb.png` = 7 209 128 B = 14 086 sektor → 12.08 s / 14 086 ≈ **858 µs/sektor**
  (≈ 6.9 ms per blok 4 KB).
- `small512.png` 46 KB → 32.4 ms = 1.4 MB/s; `logo.png` → 1.6 MB/s.

Konsekuensinya: **semua file PNG terasa "berat" bukan karena isinya, tapi karena
jalur bacanya.**

### 5.2 Empat kali *path walk* + buffer bounce + copy penuh (`kernel/syscall.c`, `kernel/fs/kfs_shim.c`)

Untuk satu `png_decode(filename)`:

| Langkah | Kode | Walk? |
|---|---|---|
| `sys_file_size` | `kernel/syscall.c:303` → `kfs_get_file_size` (`kfs_shim.c:29`) → `shim_stat` → `kfs_walk` | walk #1 |
| `sys_read_file_to_buffer` → ukuran lagi | `kernel/syscall.c:316` → `kfs_get_file_size` | walk #2 |
| → baca | `kfs_shim.c:70` → `shim_stat` → `kfs_walk` | walk #3 |
| → baca (lagi) | `kfs_shim.c:74` → `kfs_walk` | walk #4 |
| kmalloc buffer bounce seukuran file | `kernel/syscall.c:321` → `char* bounce = (char*)kmalloc(fsize);` | — |
| baca disk → bounce, lalu copy ke buffer user | `kernel/syscall.c:323-325` | — |
| `kfree(bounce)` | `kernel/syscall.c:327` | — |

Terukur: file 46 KB membaca 32 ms, dan **`sys_file_size` dingin sendiri 11.8 ms**.
Setelah hangat (bcache) `sys_file_size` = 82 µs, jadi biaya walk itu memang I/O
inode, bukan CPU. Empat walk × file besar = 4 kali menelusuri direktori untuk
satu file.

### 5.3 Decode stb_image (CPU murni)

| Sample | pixel | waktu stbi | per pixel |
|---|---|---|---|
| `small512.png` | 262 144 | 26.6 ms | 101 ns |
| `kyuzen.png` (palet 4-bit) | 1 048 576 | 78.5 ms | 75 ns |
| `logo.png` (RGBA) | 1 048 576 | 319.7 ms | 305 ns |
| `big6mb.png` | 2 304 000 | 1223.7 ms | 531 ns |

Build-nya `STBI_ONLY_PNG`, `STBI_NO_SIMD`, dan `-mno-sse`/`-msoft-float`
(`user_apps/Makefile`) — semuanya benar untuk bare-metal, tapi artinya inflate +
unfilter + konversi RGBA berjalan skalarnya. Untuk PNG RGBA penuh, ~300–530 ns
per pixel adalah biaya CPU nyata yang tidak bisa dihilangkan tanpa mengubah
arsitektur (mis. SIMD/`-msse2` jika CPU dianggap mendukung, atau decode
bertahap dengan progress indicator).

### 5.4 Alokator user (uheap): ~2.4–2.6 ms per MB

`kernel/uheap.c:35` `uheap_alloc`:

```
kernel/uheap.c:50    phys_addr_t pa = pmm_alloc_page();              // per 4 KB
kernel/uheap.c:58    memset((void*)(pa + hhdm_offset), 0, 4096);     // zero per halaman
kernel/uheap.c:66    t->uheap_brk = brk + pages * 4096 + 4096;       // brk HANYA MAJU
```

Diukur di bare metal (bukan estimasi):

| Ukuran | Waktu | µs/MB |
|---|---|---|
| 4 KB | 196 µs | 50 176 |
| 64 KB | 320 µs | 5 120 |
| 256 KB | 661 µs | 2 644 |
| 1 MB | 2 615 µs | 2 615 |
| 4 MB | 9 793 µs | 2 448 |
| 16 MB | 37 026 µs | 2 314 |
| 48 MB | **120 182 µs** | 2 503 |
| 200 × 512 B | 3 197 µs total | 15 µs/alloc |

Dua fakta dari pengukuran ini:

1. **~2.4–2.6 ms per MB** (map + zero setiap halaman 4 KB). Buffer output
   4 MB untuk PNG 1024² karena itu murni 8–10 ms; PNG 2400×1600 (9.2 MB)
   ≈ 22 ms.
2. **Semua alamat baru, tidak ada yang dipakai ulang**: 200 alokasi 512 B
   menempati alamat 341 147 648 → 342 777 856 = 1 630 208 B untuk 199 alokasi
   = **8 192 B per alokasi 512 B** (1 halaman + 1 halaman guard). `uheap_free`
   mengembalikan frame ke PMM tapi `uheap_brk` tidak pernah mundur
   (`UHEAP_BASE` 0x10000000 … `UHEAP_END` 0x40000000, `include/uheap.h:25-28`).
   Untuk viewer galeri yang memanggil `set_file()` berkali-kali
   (`image.hpp`: `png_free(px); px = png_decode(...)`), **setiap gambar yang
   dibuka menghabiskan ruang alamat baru** — dan akhirnya gagal (`uheap_alloc`
   → 0 → gambar tidak muncul), di luar hitungan cache.

Yang penting: alokasi ini **bukan** penyebab utama "terasa berat" (<1% untuk
file besar), tapi dia (a) membuat pemborosan buffer nyata dan (b) punya batas
yang bisa habis setelah membuka banyak gambar.

### 5.5 Dua buffer pixel penuh + pass konversi byte-per-byte (`apps/png.c`)

```
apps/png.c:37   uint8_t* px = stbi_load_from_memory(raw, fsize, &iw, &ih, 0, 4);  // RGBA, iw*ih*4
apps/png.c:41   uint32_t* out = (uint32_t*)sys_alloc(iw * ih * 4);                // buffer KEDUA
apps/png.c:43   for (int i = 0; i < iw * ih; i++) { ... }                         // 1 pass per byte
```

stb sudah mengembalikan buffer penuh, lalu `png.c` mengalokasikan **array kedua
seukuran**, menyalin byte-per-byte ke XRGB8888 (3.1 ms/MB terukur), lalu
membebaskan yang pertama. Puncak memori satu decode (contoh `big6mb.png`):
raw 7.2 MB + stb 9.2 MB + out 9.2 MB ≈ **25 MB** (+7.2 MB bounce kernel, transien).
Konversi ini sendiri murah; yang mahal kombinasi "alokasikan buffer baru" +
"pass penuh kedua" pada data yang sudah ada.

### 5.6 Render: 16.6 ns/pixel, dihitung ulang setiap frame (`core/painter.hpp:85`)

```
painter.hpp:92      int sy = py * ih / h;
painter.hpp:94          int sx = q * iw / w;          // pembagian integer PER PIXEL
painter.hpp:95          win->canvas[(y + py) * cw + x + q] = px[sy * iw + sx];
```

Terukur pada frame pertama `kyuzen.png` (window 1024×768):

| Metrik | Nilai |
|---|---|
| `ui_window_create` (canvas 1024×768 + window KWM) | 25.2 ms |
| decode lewat `Image` ctor (baca 169.1 + stbi 74.5) | **262.9 ms** |
| frame pertama (`render()` + flush) | 28.2 ms |
| — di dalamnya `Painter::image()`: 1 panggil, 351 649 px | **5.85 ms → 16.6 ns/px** |

Jadi render **bukan** bottleneck untuk viewer (area lihat 446×374 = 166 804 px
≈ **2.8 ms/frame**), tapi biayanya dihitung ulang tiap frame dan tanpa cache
bitmap hasil skala, jadi zoom/geser membayar penuh tiap frame
(1920×1200 penuh ≈ 38 ms/frame).

---

## 6. Cache FS tidak menolong untuk PNG

`include/bcache.h:30` → `BCACHE_N_BLOCKS 256` = **1 MB**. Efeknya terukur jelas:

| Sample | read pass 1 (dingin) | read pass 2 (langsung sesudahnya) |
|---|---|---|
| `small512.png` 46 KB | 32 372 µs | **500 µs** (65× lebih cepat) |
| `kyuzen.png` 179 KB | 118 712 µs | **2 039 µs** (58× lebih cepat) |
| `logo.png` 1.36 MB | 848 626 µs | 1 503 711 µs (**lebih lambat**) |
| `big6mb.png` 6.88 MB | 12 082 807 µs | 7 715 345 µs |

File ≤ 1 MB memanfaatkan cache dengan sangat baik; begitu file **> 1 MB**,
isinya tidak muat di cache, jadi setiap pembukaan membayar disk lagi. `logo.png`
(1.36 MB) bahkan lebih lambat di pass 2 — variansi host/emulasi ±80%, tapi
intinya jelas: **buka ulang gallery viewer = baca disk ulang penuh.**

---

## 7. Temuan struktural (TIDAK diperbaiki — di luar scope task ini)

1. **Decode resolusi penuh untuk tampilan kecil.**
   `user_apps/widget_demo.c:279` → `ui_image_create(win, "kyuzen.png", 64, 64)`:
   PNG 1024×1024 (1 048 576 px, buffer 4 MB) di-decode penuh untuk ditampilkan
   64×64 (4 096 px) → **99.6% kerja dibuang**.
   `user_apps/viewer.c:35-36` (`IMG_W` = 446, `IMG_H` = 374) + `viewer.c:172-175`
   (`ui_image_set_file` lalu `ui_image_set_fit`): gambar 1920×1200 ditampilkan
   446×278 → **94.6% pixel hasil decode dibuang**. Tidak ada jalur
   "decode/scale-down hanya sebesar yang ditampilkan".
2. **PNG > 8 MB mustahil dibuka, gagal senyap** (§4). `png.c:34` membuang
   nilai balik `sys_read_file_to_buffer`.
3. **Empat path walk + buffer bounce kernel seukuran file + copy penuh** untuk
   satu kali baca file (§5.2). Untuk PNG 8 MB: 8 MB `kmalloc` + 8 MB copy +
   8 MB `sys_alloc` = tiga kali lintas memori untuk data yang sama.
4. **`uheap` hanya maju, tidak pernah memakai ulang ruang alamat** (§5.4) +
   8 KB ruang alamat per alokasi sekecil apa pun. Viewer galeri yang membuka
   banyak gambar akan menguras 0x10000000–0x40000000 dan mulai gagal.
5. **Dua buffer pixel penuh + satu pass byte-per-byte** (§5.5).
6. **Blit tanpa cache hasil skala** (§5.6): biaya render ∝ pixel *tampilan*
   × 16.6 ns di setiap frame; zoom/geser mahal.
7. **`Image` decode di dalam constructor, sinkron, tanpa progress**
   (`libs/widget/include/primitives/image.hpp`) — untuk `big6mb.png` itu
   13.4 detik membeku; `Window::run` tidak bisa menggambar apa pun sebelum
   selesai.
8. **File manager bukan penyebab**: `user_apps/fileman.c:49-56` tidak
   men-decode apa pun — untuk `.png` dia hanya menulis **nama file** ke
   `view.tmp` lalu `sys_exec("viewer.elf")`. Tidak ada thumbnail per file.
   Begitu juga `user_apps/desktop.c` (wallpaper digambar manual, bukan lewat
   `Image`/`png_decode`), dan tidak ada app kernel-side yang men-decode PNG.

---

## 8. Kalau nanti mau dioptimasi (urutan berdasar data ini)

Hanya saran, bukan pekerjaan task ini — diurutkan dari rasio untung/risiko
terbesar menurut angka di atas:

1. **Perbaiki jalur baca blok**: `READ MULTIPLE`/DMA (satu perintah untuk 8–128
   sektor) dan/atau read-ahead. Ini 42–90% dari waktu; satu perubahan di
   `drivers/ata.c` + `bcache` menyentuh semua pembacaan file, bukan cuma PNG.
2. **Perbesar / sesuaikan `bcache` untuk file besar** (256 blok = 1 MB hari ini).
3. **Perbaiki jalur baca KyuzenFS**: hilangkan 2–4 *path walk* berulang dan
   buffer bounce seukuran file di `kernel/syscall.c:309-328`;
   periksa nilai balik `sys_read_file_to_buffer` di `png.c:34` (bug senyap).
4. **Decode sesuai ukuran tampil** (minimal: tolak/jangan penuh kalau display
   << natural size, atau sediakan jalur thumbnail) — menghapus 94–99% kerja
   pada kasus nyata `widget_demo`/`viewer`.
5. **Hindari buffer kedua + pass konversi** (minta stb memberi format yang
   langsung dipakai, atau konversi in-place).
6. **Cache bitmap hasil skala di `Painter`/`Image`** untuk zoom/geser.
7. **Progress indicator / decode asinkron** untuk file besar, karena
   `Image` ctor memblokir UI thread selama detik-an.

---

## 9. Konfirmasi revert

Self-check sebelum laporan ini dikirim:

```
$ git status --short
(kosong)
$ git diff --stat
(kosong)
$ git log --oneline -1
18389eb docs(widget-split): tandai 4 bug yang sudah diperbaiki + hapus folder libs/widged/
$ ls -la disk.img
-rw-r--r--  104857600  19 Sep 20:54 disk.img      # ukuran + mtime tidak berubah
$ make -C user_apps clean
```

Yang dipakai sementara lalu **dihapus/di-revert**:

| Artefak sementara | Nasib |
|---|---|
| instrumentasi di `apps/png.c` | `git checkout --` → kembali persis |
| instrumentasi di `libs/widget/include/core/painter.hpp` | `git checkout --` |
| definisi global di `libs/widget/src/core/painter.cpp` | `git checkout --` |
| aturan build `_pngprof.elf` di `user_apps/Makefile` | `git checkout --` |
| `user_apps/_pngprof.c` (+ `.o`/`.d`) | dihapus |
| `test/_kzfs_img_tmp.c` + `.exe` (alat host) | dihapus |
| `test/_pngprof_run.py` (runner QEMU) | dihapus |
| `test/_pngprof_tmp/` (3 sample + prof.txt + serial.log + screendump) | dihapus |
| `build/_pngprof.elf`, `libs/widget/build/**` berinstrumentasi | dihapus + `make -C user_apps clean` |
| `test_disk_prof.img` (salinan disk untuk profiling) | dihapus |

`disk.img` asli tidak pernah ditulis (semua penyuntikan ke salinannya).

Reproduksi (kalau nanti perlu diulang): lihat §2 — pola `test/_ui_probe.py` +
pola mock FS `test/kyuzenfs_xcheck.c`; tak ada dependensi baru yang perlu
di-install.
