# Proposal Desain: Desain Ulang Jalur Baca ATA

> **STATUS: FASE DESAIN — BELUM ADA SATU BARIS KODE YANG DIUBAH.**
> Dokumen ini murni analisis + proposal + rencana test + rencana rollback.
> Belum ada yang ditulis/disentuh di `drivers/`, `kernel/`, atau file lain.
> Bukti: `git status` hanya berisi dokumen ini (+ laporan profiling sebelumnya
> yang belum di-commit), `disk.img` tidak berubah —
> sha256 `1426eeb4187723239875e7f6c5a56861ee50c3ddd99401dc77e762f6f9adcf92`
> (100 MiB / 204 800 sektor / 25 600 blok 4 KB), tanggal berjalan `18389eb`.

Basis data: `DOCUMENTATION/design/png-decode-profile.md` (profiling bare-metal,
QEMU headless, TSC terkalibrasi 2.61 GHz). Dokumen ini **tidak mengukur ulang**;
semua angka yang dikutip berasal dari laporan itu. Yang baru di sini adalah
pembacaan kode dengan nomor baris, probe QEMU (read-only), dan analisis biaya.

---

## 1. Ringkasan untuk reviewer (TL;DR)

### 1.1 Tiga koreksi terhadap asumsi di prompt

**(a) Tidak ada KVM — ini TCG, jadi "VM exit per `inb`/`inw`" secara harfiah
tidak ada.** `make run` memanggil `qemu-system-x86_64.exe` **tanpa**
`-enable-kvm`/`-accel` (Makefile:487-494). Semua port I/O dieksekusi sebagai
helper TCG di dalam proses host. Implikasi: biaya per operasi port adalah
"eksekusi instruksi guest + helper + dispatch device", bukan exit KVM. Angka
"~858 µs/sektor" tetap valid sebagai **hasil ukur**, tapi penjelasan
mekanismenya harus diganti — dan itu penting, karena pilihan opsi bergantung
pada komponen mana yang dominan (lihat §3).

**(b) Opsi 1 (`READ MULTIPLE`) bukan kandidat "menang besar" yang bisa
diasumsikan.** Dari pembacaan kode, setiap sektor = 272 operasi port, dan
**256 di antaranya (94,1%) adalah kata data PIO** (`inw` di `drivers/ata.c:75-77`).
Batching hanya menghapus operasi *non-data* (maksimum 5,9% dari total operasi
per blok 4 KB, lihat §3.1). Selama jalur data tetap PIO, batchnya tidak
mengurangi satu kata pun. Uji konsistensi terhadap tiga sampel profil
menunjukkan biaya per operasi ≈ konstan **1,15–1,32 µs/op** (§3.2) — artinya
yang diukur memang *didominasi* kata data, bukan overhead per-command. Jadi
plafon realistis opsi 1 di lingkungan dev ini adalah **≈ 5%**, kecuali
prototipe membuktikan biaya per-command jauh lebih mahal daripada per-kata
(§6, P2/P3).

**(c) Ada opsi yang tidak disebut di prompt, lebih murah dari DMA dan tetap
PIO: `rep insw` (string I/O).** 256 `inw` dalam loop C diganti **satu
instruksi** `rep insw`. Tetap perintah `READ SECTORS` yang sama, tetap polling,
tanpa IRQ, tanpa alokasi, tanpa PCI — jadi tetap aman untuk jalur panic
(`crashdump.c` mensyaratkan polling tanpa interrupt, `kernel/crashdump.c:9-11`).
Kehati-hatian: butuh helper baru di `include/io.h` (belum ada `insw`/`outsw`,
lihat `include/io.h:1-39`) dan **wajib `cld` sebelum `rep`** (kalau DF=1, data
ditulis mundur → korupsi memori). Perkiraan gain di TCG: kecil–sedang (§4.3),
tapi di hardware asli ini jalur PIO cepat yang kanonik (2–4×).

### 1.2 Kesimpulan utama

1. Masalah strukturalnya **bukan** "satu perintah per sektor" saja, tapi
   "**satu operasi port per 2 byte data**". PIO 16-bit tidak bisa dihindari
   selama data masuk lewat port `0x1F0`. Di QEMU/TCG setup ini, itu berarti
   **plafon PIO ≈ 1,7 MB/s** (2 byte ÷ 1,2 µs/op) — konsisten dengan ukuran
   0,57–1,6 MB/s di laporan profiling.
2. Karena itu: **hanya bus-master DMA yang menghapus biaya per-kata** (device
   memindahkan data tanpa kerja vCPU). `rep insw` dan batching hanya
   memangkas sebagian kecil komponennya **di QEMU**; di hardware asli keduanya
   tetap berarti (batching + string I/O → PIO ~16 MB/s, DMA 33–100+ MB/s).
3. Tapi memutuskan pindah ke DMA sekarang = menerima prasyarat yang **hari ini
   tidak ada**: PCI config *write* (bus-master enable), pencarian BAR4,
   PRDT, alamat fisik untuk buffer DMA (heap kernel ada di
   `0xFFFF900000000000`, di luar HHDM — `kernel/heap.c:40-47`; preseden
   `pmm_alloc_page + hhdm_offset` ada di `graphics/memory/gpu_alloc.c:9-31`),
   plus verifikasi bahwa polling BMDMA benar-benar selesai di TCG tanpa IRQ
   (§6, P4). Ini perubahan arsitektur, bukan tweak lokal — dan **IRQ14 tidak
   boleh dipakai** (tidak ada gate di IDT, lihat §2.1).

### 1.3 Rekomendasi urutan (detail + gerbang keputusan di §5)

```
Stage 0  Prototipe pengukuran (sekali pakai, tanpa tulis ke disk.img)   ← WAJIB dulu
Stage 1  PIO batching (count>1) + rep insw, di balik flag compile-time  ← risiko kecil
Stage 2  Bus-master DMA polling (hanya kalau Stage 1 tidak cukup)       ← perlu go/no-go
Stage 3  Read-ahead bcache + jalur tulis (flush-per-sektor)             ← ticket terpisah
```

### 1.4 Keputusan yang saya minta dari Anda

Sebelum implementasi apa pun: jawab **§9 Pertanyaan terbuka** (10 butir).
Yang paling menentukan: (1) target QEMU/TCG saja atau hardware asli juga,
(2) boleh tidak `rep insw`, (3) bounce buffer vs API `virt_to_phys` baru,
(4) boleh tidak mengubah signature `ata_read_sector` (`void` → `int`).

---

## 2. Riset wajib (poin 1–5), dengan bukti

### 2.1 Infrastruktur interrupt yang ada (dan kenapa DMA+IRQ itu mahal)

| Fakta | Bukti |
|---|---|
| IDT hanya punya gate untuk IRQ0 (timer, vektor 32), IRQ1 (keyboard, 33), IRQ12 (mouse, 44), LAPIC timer (240) | `arch/x86/idt.c:83-87` |
| **Tidak ada gate untuk IRQ14/vektor 46** di mana pun | `grep IRQ14` seluruh tree = 0 hasil |
| PIC di-remap + di-mask, tapi kodenya **ada di driver keyboard**, bukan file PIC | `drivers/keyboard.c:63-67` |
| Mask master `0xF8` → IRQ0/1/2 unmask; mask slave `0xEF` = `1110_1111` → **IRQ14 (bit 6) tertutup** | `drivers/keyboard.c:67` |
| Handler EOI hanya ke master (`outb(0x20,0x20)`) | `drivers/keyboard.c:82,190` |
| Tidak ada API registrasi IRQ generik / helper unmask | `grep pic_unmask\|irq_register\|enable_irq` = 0 hasil |
| Sistem SMP (`-smp 8`) + LAPIC dipakai untuk tick AP | Makefile:489; `arch/x86/idt.c:87`, `arch/x86/lapic.c` |

**Konsekuensi:** DMA yang mengandalkan interrupt IDE butuh: gate IDT baru +
stub ISR + helper unmask slave + EOI ke **dua** PIC + keputusan routing di SMP.
Kalau salah satu lupa, bukan "I/O lambat" — tapi **#GP / panic** saat interrupt
pertama datang. Ditambah: prinsip crashdump (`kernel/crashdump.c:9-11`,
"POLLING ATA PIO — tidak ada interrupt") **melarang** interrupt di jalur panic.
**Kesimpulan: DMA harus polling BMDMA, bukan IRQ** (§4.4/§4.5).

### 2.2 Kapabilitas ATA yang dipakai QEMU sekarang

Konfigurasi `make run` (Makefile:487-494): `-cpu max -m 1G -boot d -smp 8`,
**tidak ada `-enable-kvm`** (→ TCG), `-drive file=disk.img,format=raw,index=0,media=disk`,
`-drive file=build/boot_image.iso,media=cdrom,index=2`.

Probe QEMU monitor read-only (device model, bukan boot): QEMU **10.2.1**;
`piix3-ide` (`8086:7010`, PIIX3) dengan **`BAR4` I/O ada** → **BMDMA bus-master
tersedia**; `ide-hd` di `ide.0` = `disk.img` (read-write), `ide-cd` di `ide.1`
= ISO. Properti `ide-hd` di 10.2.1 **tidak lagi punya `multiwrite`** (dulu ada);
`piix3-ide` juga tidak mengekspos properti `bmdma` — jadi **kapabilitas
per-perintah (word 59 multi-sector, word 49/63/88 DMA) wajib dikonfirmasi lewat
IDENTIFY di prototipe P1 (§6)**, jangan diasumsikan dari properti device.

Catatan penting untuk angka: `-m 1G`/`-smp 8`/TCG berarti **angka absolut
QEMU ≠ hardware asli**. Laporan profiling sudah menyatakan ini; dokumen ini
menyimpan kualifikasi yang sama: struktur masalah (1 perintah per 512 B, tanpa
DMA) nyata di hardware, angkanya tidak.

### 2.3 LBA28 vs LBA48

| Fakta | Bukti |
|---|---|
| Jalur baca/tulis murni **LBA28** (`0xE0 \| ((lba>>24)&0x0F)`) | `drivers/ata.c:49` (baca), `:85` (tulis) |
| IDENTIFY **sudah** mendeteksi dukungan LBA48 (word 83 bit 10, word 100-103) | `drivers/ata.c:143-152` |
| Tapi hasilnya di-clamp ke 32-bit (API publik `uint32_t`) | `drivers/ata.c:150-151`, `include/ata.h:9` |
| `disk.img` sekarang = 100 MiB = 204 800 sektor = **25 600 blok 4 KB** | `stat` disk.img |
| Batas LBA28: 2^28 sektor = **128 GiB** | spesifikasi ATA |
| Cast `(uint32_t)(block_num * 8)` overflow baru pada ≥ 2 TiB | `drivers/ata.c:169,179` |

**Kesimpulan:** LBA28 vs LBA48 **bukan lubang aktif** untuk `disk.img` 100 MiB
(limit 128 GiB masih 1300× lebih jauh). Tapi karena ini rewrite, primitif baru
sebaiknya LBA48-ready; dan kalau nanti ada image > 128 GiB, `ata_read_sector`
yang sekarang akan diam-diam membaca LBA yang salah (bukan error) — ini dicatat
sebagai temuan, bukan diperbaiki di sini (§4.8).

### 2.4 `bcache`: di dalam atau di luar scope?

**Rekomendasi: DI LUAR scope task ini** (sesuai default prompt), dengan alasan:

1. `bcache` sudah punya tanggung jawab sendiri (LRU 256 × 4 KB = 1 MB,
   `include/bcache.h:30-38`) dan memanggil `ata_read_block4k` **satu blok per
   panggilan** (`kernel/fs/bcache.c:184`). Read-ahead = fitur baru di lapisan
   itu, bukan bagian dari "ganti cara baca sektor".
2. Menggabungkannya berarti satu PR menyentuh dua lapisan + kebijakan caching
   (mis. apakah prefetch boleh mengusir blok pinned?) — memperbesar blast
   radius justru di tempat yang paling sulit di-review.
3. Yang **tetap** dilakukan sekarang: buat primitif ATA menerima **rentang
   LBA + jumlah sektor** (bukan hanya "1 blok"), sehingga read-ahead nanti
   cukup satu panggilan tanpa mengubah driver lagi. Desain-kan sekarang,
   implementasi read-ahead tetap ticket terpisah (Stage 3).

**Temuan sampingan (jangan diperbaiki di task ini):** header `include/bcache.h:19-22`
mendokumentasikan "I/O disk dilakukan DI LUAR lock", tapi kode aktual
`kernel/fs/bcache.c:179-184` **memegang `bcache_lock` selama `ata_read_block4k`**
dengan komentar yang menyatakan itu keputusan sadar ("kita PERTAHANKAN lock di
sini ... sederhana & benar"). Artinya: (a) baca disk terserialisasi lewat
`bcache_lock` — bagus untuk konkurensi ATA (lihat §4.8), tapi (b) spinlock
`irqsave` dipegang selama ± 2,9–6,9 ms PIO per blok 4 KB → interrupt mati
selama itu. Di QEMU tidak terasa; di hardware asli itu bug latensi serius.
Cukup dicatat; dokumen `bcache.h` dan kodenya perlu diselaraskan di ticket
terpisah.

### 2.5 Siapa saja pemanggil API ATA saat ini (blast radius)

| Pemanggil | Baris | Catatan |
|---|---|---|
| `kernel/crashdump.c` | `:75`, `:122`, `:134`, `:237`, `:243` | pakai `ata_read_sector`/`ata_write_sector` **langsung**, tanpa lock, konteks panic |
| `kernel/fs/bcache.c` | `:26-27` (extern), `:121`, `:184`, `:210`, `:228`, `:243` | `ata_read_block4k`/`ata_write_block4k`; satu-satunya jalur data normal |
| `kernel/fs/kfs_super.c` | `:154` | `ata_get_total_sectors()` (IDENTIFY) saat mount |
| `kernel/kernel.c` | `:345-347` | IDENTIFY saat boot → `crashdump_init(total - 8)` |
| `legacy/kernel/kyuzenfs_v3.c` | banyak | **dead code — tidak dibangun** (tidak ada referensi `legacy` di Makefile) |
| `tools/mkfs.kyuzenfs.c` | `:96` | hanya komentar |
| Mock host (harus ikut berubah kalau signature berubah) | `test/panic_test.c:192-233`, `test/kyuzenfs_v4_test.c`, `test/kyuzenfs_dir_test.c`, `test/kyuzenfs_xcheck.c:19-40` | masing-masing mendefinisikan `ata_*` sendiri |

Bootloader **tidak** memakai `drivers/ata.c` (Limine membaca ISO lewat BIOS/UEFI),
jadi boot-from-ISO tidak terpengaruh. Yang terpengaruh saat bring-up kernel:
IDENTIFY (`kernel.c:345`) dan mount KyuzenFS + semua baca file lewat bcache.
Bentuk API publik saat ini (`include/ata.h:6-14`): `void ata_read_sector(...)`,
`void ata_write_sector(...)`, `uint32_t ata_get_total_sectors(void)`,
`int ata_read_block4k(uint64_t, void*)`, `int ata_write_block4k(...)`.

---

## 3. Analisis biaya: dari mana waktu per sektor berasal

### 3.1 Op-count per sektor dan per blok 4 KB

Setiap `ata_read_sector` (`drivers/ata.c:44-78`) melakukan:

| Tahap | Operasi port | Baris |
|---|---|---|
| `ata_wait_bsy()` | ≥ 1 × `inb` (loop, maks 100 000) | `:25-31`, dipanggil `:46` |
| Pilih drive (LBA28) | 1 × `outb` | `:49` |
| Delay 400 ns spec | 4 × `inb` (Alt Status) | `:17-22`, `:50` |
| Set count = 1 | 1 × `outb` | `:53` |
| Set LBA lo/mid/hi | 3 × `outb` | `:56-58` |
| Perintah `READ SECTORS` (0x20) | 1 × `outb` | `:61` |
| Delay 400 ns | 4 × `inb` | `:64` |
| `ata_wait_drq()` | ≥ 1 × `inb` | `:34-42`, dipanggil `:67` |
| **Transfer data** | **256 × `inw`** | **`:75-77`** |

**Total minimum: 272 operasi port per sektor**, dengan **256 (94,1%) adalah
kata data PIO**. Per blok 4 KB (`ata_read_block4k` memanggil 8× sektor,
`:166-174`): **2 176 operasi**, 128 di antaranya non-data (**5,9%**).

Semua hitungan gain di §3.4 dan §4 memakai basis ini — bukan angka karangan,
tapi hitungan langsung dari kode.

### 3.2 Uji konsistensi: apakah angka profiling cocok dengan model op-count?

Kalau biaya per operasi port seragam, maka `µs/op = waktu_baca / (sektor × 272)`.
Hasilnya:

| Sampel (dari laporan profiling, tabel mentah §3) | byte | sektor | t_baca | µs/sektor | total op | **µs/op tersirat** |
|---|---|---|---|---|---|---|
| `small512.png` | 46 077 | 90 | 32 372 µs | 359,7 | 24 480 | **1,32** |
| `kyuzen.png` | 182 832 | 357 | 118 712 µs | 332,5 | 97 104 | **1,22** |
| `logo.png` | 1 394 920 | 2 725 | 848 626 µs | 311,4 | 741 200 | **1,15** |
| `big6mb.png` | 7 209 128 | 14 081 | 12 082 807 µs | 858,1 | 3 830 032 | **3,15** ⚠️ |

**Tiga sampel pertama konsisten di 1,15–1,32 µs/op** (sebaran 13%). Artinya
seluruh waktu baca ketiga file itu — termasuk per-command, poll, delay, dan
kerja device — **bisa dijelaskan sebagai "jumlah operasi port × 1,2 µs"**.
Ini bukti kuat bahwa yang dominan adalah **kata data PIO** (94% op), bukan
overhead per-perintah.

**`big6mb.png` adalah outlier 2,6× lebih lambat per-op** — dan ini penting,
bukan detail. Laporan profiling sendiri menandai variansi ±80% antar-pass dan
`logo.png` pass-2 lebih lambat dari pass-1, jadi kandidat penjelasannya: urutan
pengukuran dalam satu sesi (beban host bertambah), efek host page-cache/read
ahead, atau bcache. **Tidak bisa dipisahkan dari data yang ada** → prototipe
P2/P3 (§6) yang harus memutuskan, bukan tebakan.

Implikasi turunan: dari 1,2 µs/op dan 1 op per 2 byte, **plafon PIO di setup
inI ≈ 1,7 MB/s**. Angka laporan (0,57–1,6 MB/s) persis di bawah plafon itu.
Batching/string I/O memangkas sebagian kecil dari sisi *op count non-data*,
tapi tidak mengubah "1 op per 2 byte" selama data lewat port 16-bit.

### 3.3 Hipotesis faktor yang belum dijelaskan (harus diuji, jangan ditebak)

| Hipotesis | Cara prototipe membedakannya |
|---|---|
| **H1 — Biaya per-command QEMU besar** (host read per perintah, setup transfer): batching menang besar | P2 arm B (count=8) vs A (baseline) pada file sama; P3 mencatat jumlah iterasi poll + hitung selisih |
| **H2 — Biaya per-kata data dominan** (TCG helper + PIO word path): batching ≲ 6%, hanya DMA/string I/O yang menolong | P2 arm C (`rep insw`) vs A; ukur selisih per kata, bukan per perintah |
| **H3 — Poll loop iterasi banyak** (BSY/DRQ perlu waktu device): batching menolong karena poll ikut berkurang 8× | P3: counter iterasi `wait_bsy`/`wait_drq` per sektor |
| **H4 — Outlier big6mb artefak sesi/host** (bukan sifat jalur ATA) | P2: ukur ulang 4 file dengan **N=5 interleaved** (bukan satu per satu berurutan), laporkan median + min |

### 3.4 Plafon tiap opsi menurut model (batas atas, bukan janji)

| Opsi | Op yang hilang per blok 4 KB | Plafon model (TCG, uniform 1,2 µs/op) | Catatan |
|---|---|---|---|
| Batching count=8 (opsi 1a/1b) | 128 → ~17 non-data (**−111 op**) | **≈ 5,9% ≈ 1,06×** | hanya kalau H1/H3 benar bisa lebih besar |
| `rep insw` (opsi 1c) | 2 047 op jadi ~1 instruksi, tapi **loop per-kata di dalam helper TCG tetap jalan** | kecil–sedang di TCG; **2–4× di hardware asli** | yang dihapus: dispatch instruksi guest + overhead loop C, bukan kerja device |
| DMA polling (opsi 2) | 2 048 kata + ~128 non-data → ~20 op + 1 memcpy 4 KB | **5–100×** (dibatasi I/O host) | satu-satunya yang menghapus kerja per-kata device |
| Read-ahead (opsi 3) | tidak mengurangi op per byte; mengurangi jumlah *perintah* per byte | tergantung pola akses | di luar scope (§2.4) |

⚠️ Semua angka di tabel ini **turunan model**, bukan hasil ukur. Yang mengukur
hanya prototipe §6. Saya menandainya eksplisit supaya tidak dipakai sebagai
justifikasi sendiri.

---

## 4. Opsi (wajib dievaluasi + satu tambahan hasil riset)

### 4.1 Opsi 1a — satu perintah `READ SECTORS` untuk 8 sektor (paling murah)

**Cara kerja:** `ata_read_block4k` mengirim **satu** perintah `0x20` dengan
`SECTOR_COUNT = 8`, lalu membaca 8 × 512 byte (DRQ per sektor), bukan 8 perintah
terpisah. `READ SECTORS` dengan count 1–255 **wajib** ada di semua device ATA
(tidak butuh bit kapabilitas apa pun), jadi ini opsi paling aman secara protokol.

- **Gain:** menghapus 7× (wait_bsy + select + 8 delay-inb + set count/LBA +
  perintah + wait_drq) per blok → −111 op dari 2 176 (**≈ 5,9%** di model
  uniform). Bisa lebih kalau H1/H3 (§3.3) benar.
- **Kompleksitas:** rendah–sedang. Hati-hati: (i) transfer harus dicek ERR/DF
  **per sektor**; (ii) DRQ re-assert antar sektor harus di-poll; (iii) timeout
  total, bukan per-perintah; (iv) `lba0 + count` tidak boleh melewati batas disk.
- **Risiko:** rendah (protokol dasar, tidak ada mode baru). Risiko utama: bug
  off-by-one LBA/sector → **salah baca** (bukan error) → test §7.1 wajib.
- **Di luar `ata.c`:** tidak ada. Tidak butuh IRQ, PCI, alokasi, atau API baru.

### 4.2 Opsi 1b — `READ MULTIPLE` (0xC6)

**Cara kerja:** perintah 0xC6 dengan count = ukuran blok multi (word 59
IDENTIFY), device melakukan transfer multi-sektor dengan IRQ per blok.

**Penilaian: tidak lebih baik dari 1a untuk kasus ini.** (i) Butuh validasi
word 59 (jumlah yang valid 1..max), lalu fallback kalau tidak didukung;
(ii) manfaat tambahannya (DRQ/IRQ per blok, bukan per sektor) **tidak terasa
saat polling** — kita tetap membaca 512 byte per DRQ; (iii) di QEMU 10.2.1
properti `multiwrite` sudah tidak ada, jadi dukungannya harus dikonfirmasi
dulu (§6 P1). Kesimpulan: **implementasikan 1a; 1b hanya kalau P1 menunjukkan
ada perbedaan terukur** (mis. device menangani multi-blok jauh lebih efisien).

### 4.3 Opsi 1c — string I/O `rep insw` (temuan tambahan)

**Cara kerja:** loop 256 × `inw` (`drivers/ata.c:75-77`) diganti satu
instruksi `rep insw` (port `0x1F0`, count 256, tujuan buffer). Protokolnya
**sama persis** (tetap `READ SECTORS` count=1), jadi tidak ada mode/kapabilitas
baru yang perlu dicek.

- **Gain di TCG:** sedang. Instruksi guest yang dieksekusi turun dari ~256 ×
  (inw + loop) jadi 1 helper, tapi helper `insw` QEMU tetap memanggil handler
  device per kata — jadi bagian "kerja device" tidak hilang. Perkiraan: puluhan
  persen, bukan belasan kali.
- **Gain di hardware asli:** 2–4× dibanding loop per-kata; `rep insw` adalah
  jalur PIO cepat yang kanonik.
- **Kompleksitas:** rendah — helper inline baru di `include/io.h` + pemakaian di
  `ata.c`. Perlu `cld` eksplisit sebelum `rep` (DF harus 0; kalau DF=1 data
  ditulis mundur → korupsi memori senyap). Panjang transfer (8 sektor = 4 KB)
  aman untuk batas satu kali `rep`.
- **Risiko:** rendah, **tetap aman untuk crashdump** (tanpa alokasi/IRQ/lock).
- **Catatan:** ini prasyarat kecil untuk Stage 2 juga (jalur `insw`/`outsw` sama
  dipakai untuk PIO fallback).

### 4.4 Opsi 2 — bus-master DMA dengan **polling** (bukan IRQ)

**Cara kerja:** program PRDT (Physical Region Descriptor Table) di memori fisik,
tulis alamat PRDT + arah ke register BMDMA (`BAR4`), lalu kirim `READ DMA`
(0xC8) ke port perintah; **poll** bit aktif di register status BMDMA sampai
selesai (tanpa interrupt sama sekali → memenuhi prinsip crashdump).

**Prasyarat yang harus diverifikasi/dibangun (semuanya baru):**

| # | Prasyarat | Status hari ini | Bukti |
|---|---|---|---|
| 1 | BMDMA ada di device | **Ada** (BAR4 I/O terdeteksi) | probe `info qtree` (read-only) |
| 2 | PCI config **write** (set bus-master enable bit di offset 0x04) | **Tidak ada** — `pci.c` cuma punya `pci_read_word` yang bahkan truncate ke 16 bit | `drivers/pci.c:9-20`, `include/pci.h:18-19` |
| 3 | Baca BAR4 32-bit | **Tidak ada helper**; harus baca dua word (0x20 + 0x22) atau tambah `pci_read_dword` | `drivers/pci.c:19` |
| 4 | Alamat fisik buffer DMA | Heap kernel di `0xFFFF900000000000` **di luar HHDM** → `virt − hhdm_offset` tidak berlaku | `kernel/heap.c:40-47`, `kernel/kernel.c:64,84,183-186`, `include/pmm.h:9` |
| 5 | PRDT di memori fisik valid | Butuh buffer dari `pmm_alloc_page()` + `hhdm_offset` (preseden sudah ada) | `graphics/memory/gpu_alloc.c:9-31` |
| 6 | Sink data = `b->data` di pool bcache (kmalloc 1 MB, **tidak** page-aligned & tidak kontigu) | butuh bounce buffer + memcpy 4 KB, atau API `virt_to_phys` baru di `paging.c` | `include/bcache.h:31-38`, `kernel/fs/bcache.c:184` |
| 7 | Polling BMDMA selesai di TCG tanpa IRQ | **Belum diverifikasi** — QEMU menyelesaikan AIO di main loop/thread iothread; wajib prototipe P4 | §6 |
| 8 | Timeout BMDMA | belum ada (timeout hari ini loop-count PIO) | `drivers/ata.c:25-42` |

- **Gain:** satu-satunya opsi yang menghapus 2 048 kata data port per blok.
  Estimasi **5–100×** di QEMU (dibatasi I/O host + latensi AIO); di hardware
  asli memindahkan beban CPU `inw` ke controller dan menaikkan plafon dari
  ~16 MB/s (PIO) ke 33–100+ MB/s.
- **Kompleksitas:** tinggi (4 lapisan: PCI config, BMDMA/PRDT, alokasi fisik,
  fallback). **Paling tidak**: 5–8 file.
- **Risiko:** tinggi. Kalau PRDT/alamat salah → device menulis ke memori
  arbitrer (bukan cuma "baca salah"). Wajib dengan flag default OFF + test
  §7 sebelum diaktifkan.
- **Di luar `ata.c`:** ya — `pci.c`/`pci.h` (config write + BAR), `ata.c`,
  opsi `paging.c` (kalau pilih rute `virt_to_phys`), plus **wajib** flag
  compile-time agar jalur PIO lama tetap ada sebagai fallback permanen.

### 4.5 Opsi 2b — DMA + IRQ14 (dievaluasi, **tidak direkomendasikan**)

Butuh: gate IDT vektor 46 + stub ISR + unmask slave PIC + EOI ganda +
penyesuaian SMP/IOAPIC **dan** melanggar prinsip "tanpa interrupt" di jalur
panic (§2.1). IRQ-driven DMA juga menambah permukaan race baru terhadap jalur
crashdump yang memanggil `ata_read_sector` langsung tanpa lock (§4.8). Semua
manfaat DMA didapat dari varian polling; IRQ hanya menghemat polling. **Tolak
untuk sekarang**; bisa ditinjau setelah Stage 2 stabil.

### 4.6 Opsi 3 — read-ahead / prefetch

Di luar scope (§2.4). Independen dari 1a/1b/1c/2 dan bisa digabung nanti.
Syarat desain yang **diambil sekarang**: primitif baru menerima
`(lba_awal, jumlah_sektor, buffer)`, bukan terikat "1 blok 4 KB", supaya
bacanya bisa sekaligus 8–32 sektor saat read-ahead ditambahkan.

### 4.7 Tabel perbandingan

| Opsi | Gain (model, QEMU) | Gain (hardware asli) | Kompleksitas | Risiko | Scope perubahan |
|---|---|---|---|---|---|
| **1a** READ SECTORS count=8 | ≈ 5,9% (bisa lebih jika H1/H3) | ≈ 5–15% | Rendah | Rendah | `drivers/ata.c` saja |
| **1b** READ MULTIPLE 0xC6 | ≈ 1a | ≈ 1a | Rendah–sedang (butuh word 59 + fallback) | Rendah | `drivers/ata.c` |
| **1c** `rep insw` | puluhan % (TCG) | **2–4×** | Rendah | Rendah (wajib `cld`) | `include/io.h` + `ata.c` |
| **2** DMA polling | **5–100×** (batas I/O host) | **3–10×+** | Tinggi | Tinggi | `pci.c/h`, `ata.c`, (opsional `paging.c`), + flag |
| **2b** DMA + IRQ14 | sama dengan 2b, tapi lebih rumit | idem | Tinggi + IDT/PIC | Tinggi | + `idt.c`, `keyboard.c`, stub ISR |
| **3** read-ahead | menggandakan efek 1a/2 | idem | Sedang | Sedang | `bcache.c` (ticket terpisah) |

### 4.8 Prasyarat lintas-opsi (temuan bug — **tidak** diperbaiki di task ini)

1. **Error dibuang total.** `ata_read_sector` `void` dan **mengisi buffer
dengan nol berisi error** (`drivers/ata.c:67-71`); `ata_read_block4k` selalu
`return 0` (`:173`). Jadi korupsi baca = "data nol" tanpa jejak. Setiap
opsi baru harus mengembalikan error — tapi itu perubahan signature yang
menyentuh `crashdump.c` + 4 mock test. **Keputusan Anda diperlukan (§9 #5).**
2. **Tidak ada lock channel ATA.** `bcache` kebetulan menyerialkan I/O lewat
`bcache_lock` (`kernel/fs/bcache.c:179-184`), tapi `crashdump.c` memanggil
port langsung (`:75,122,134,237,243`) — di sistem `-smp 8`, panic di satu CPU
sambil CPU lain membaca lewat bcache bisa menyisipkan perintah ke channel yang
sama. Hari ini peluangnya kecil (panic = sistem sudah rusak), tapi **DMA
memperburuknya** (satu engine BMDMA dibagi). Rekomendasi: `ata_lock` spinlock
di `ata.c` sebagai prasyarat Stage 1/2 — sebaiknya **PR terpisah lebih dulu**
supaya perubahan jalur baca tetap murni.
3. **Jalur tulis `CACHE FLUSH` per sektor** (`drivers/ata.c:109-112`): 8 flush
untuk satu blok 4 KB. Tidak diperbaiki di sini (read path), tapi ini
performance cliff terpisah yang layak ticket sendiri.
4. **Cast `uint32_t` pada `block_num * 8`** (`:169,179`) + LBA28: aman untuk
100 MiB, jadi lubang laten (§2.3). Catat, jangan sentuh di task ini.
5. **`bcache.h` vs `bcache.c` tidak sinkron** soal "I/O di luar lock" (§2.4).
Ticket dokumentasi terpisah.

---

## 5. Rekomendasi urutan pengerjaan (dengan gerbang keputusan)

Urutan di bawah **tidak** mengikuti asumsi prompt secara langsung; alasannya
ditulis eksplisit per stage. Ringkas: karena plafon opsi 1 di model op-count
hanya ≈ 5,9%, dan karena dua hipotesis (H1 vs H2 di §3.3) menghasilkan
kesimpulan yang **berlawanan** (batching menang vs DMA satu-satunya jalan),
maka keputusan yang benar-benar murah adalah **mengukur dulu (Stage 0)**, bukan
langsung menulis driver baru berdasarkan tebakan — milik saya maupun Anda.

### Stage 0 — Prototipe pengukuran (WAJIB, sekali pakai, tanpa tulis ke disk)

Isi: P1–P4 (§6), dijalankan pada **salinan** `disk.img`, kode instrumentasi
di-revert sebelum laporan. Keluaran: dekomposisi biaya per sektor
({perintah, kata data, poll, host I/O}) dengan ketidakpastian < 20%, plus
re-baseline 4 sampel dengan N=5 interleaved.

**Gerbang keputusan (setelah Stage 0):**

| Kalau hasil ukur menunjukkan | Maka |
|---|---|
| Kata data ≥ 70% biaya **dan** gain batching < 15% | Lompat ke Stage 2 (DMA) **setelah** 1a+1c tetap diimplementasikan sebagai peningkatan hardware nyata (murah, dan tidak boleh hilang) |
| Biaya per-command/host I/O ≥ 50% | Stage 1a saja kemungkinan sudah cukup; Stage 2 ditunda |
| `rep insw` memberi ≥ 2× di TCG | Prioritaskan 1c sebelum 2; protokolnya sama, risikonya jauh lebih rendah |
| Outlier big6mb hilang saat N=5 interleaved | Pakai median, bukan angka pass tunggal, di semua laporan berikutnya |

### Stage 1 — PIO batching (1a) + string I/O (1c), di balik flag

- Perubahan: `drivers/ata.c` + helper di `include/io.h`.
- Kedua jalur (lama & baru) **dikompilasi**, default tetap jalur lama sampai
  test §7 hijau + benchmark Stage 1 menunjukkan hasil.
- Dinilai dari: tabel benchmark yang sama persis dengan laporan profiling
  (metodologi §2 laporan itu, supaya bisa dibandingkan langsung).
- PIO lama **tidak pernah dihapus** — dia adalah fallback permanen (device tanpa
  multi-sector/string-I/O cepat, jalur panic, dan bila H1 ternyata salah).

### Stage 2 — DMA polling (opsi 2), hanya kalau Stage 1 tidak cukup

- Prasyarat 1–8 di §4.4 harus selesai/diputuskan dulu; `pci.c` config write dan
  bounce buffer adalah dua pekerjaan terbesar.
- Urutan internal: (2.1) `pci_read_dword`/`pci_write_word` + probe BAR4 read-only?
  tidak ada tulis disk; (2.2) PRDT + bounce buffer + `READ DMA` **hanya baca**,
  dibandingkan byte-per-byte dengan PIO pada image salinan (test §7.1/§7.2
  sudah tersedia); (2.3) baru pertimbangkan jalur tulis.
- **Jangan** aktifkan lewat IRQ14 (§4.5).

### Stage 3 — Read-ahead + jalur tulis (ticket terpisah, bukan bagian ini)

Read-ahead bcache memakai primitif baru (§4.6); jalur tulis memakai WRITE DMA +
satu flush per batch (menggantikan flush-per-sektor). Keduanya punya blast
radius sendiri dan tidak boleh digabung ke commit jalur baca.

---

## 6. Prototipe yang perlu divalidasi (spesifikasi)

Semua prototipe: **sekali pakai**, dijalankan pada salinan `test_disk_*.img`,
tidak boleh menulis `disk.img` asli, dan di-revert sebelum laporan
(`git diff` kosong). Metodologi ukur mengikuti `png-decode-profile.md` §2
(TSC terkalibrasi, cold/warm dipisah).

- **P1 — IDENTIFY dump (read-only).** Baca word 49 (kapabilitas DMA), 59
  (multi-sector), 63 (multiword DMA), 88 (UDMA) dari device yang dipakai
  `make run`, cetak ke COM1. Menjawab: `READ MULTIPLE` & `READ DMA` didukung?
  Butuh: fungsi kecil sementara di `ata.c` + panggilan di `kernel.c`,
  di-revert setelahnya.
- **P2 — Benchmark 3 arm (A/B/C).** Arm A = `ata_read_sector` apa adanya;
  B = satu perintah `READ SECTORS` count=8; C = B + `rep insw`. Empat file
  sampel (reuse aset → atau buat ulang seperti profiling), **N=5 interleaved**,
  laporkan median + min/max per arm. Ini yang memutuskan H1 vs H2.
- **P3 — Counter operasi.** Instrumentasi penghitung: jumlah `inb`/`inw`/`outb`,
  jumlah iterasi `wait_bsy`/`wait_drq` per pembacaan, dan jumlah sektor.
  Menjawab H3 dan memvalidasi model 272 op/sektor (§3.1) terhadap kenyataan.
  Kalau jumlah op ternyata ≠ 272 × sektor, modelnya salah dan plafon §3.4 ikut
  salah — ini penjaga integritas analisis.
- **P4 — Kelayakan DMA (read-only, image salinan).** Alokasi 1 halaman
  `pmm_alloc_page` + `hhdm_offset`, isi PRDT 1 entri, program BMDMA, kirim
  `READ DMA`, poll status, bandingkan 4 KB hasilnya dengan baca PIO. Ini
  memverifikasi prasyarat #7 (§4.4) **dan** bahwa polling selesai di TCG tanpa
  IRQ. Kalau P4 gagal, Stage 2 batal dengan biaya sangat kecil.

---

## 7. Rencana test yang WAJIB ada sebelum implementasi disentuh ke kode asli

### 7.1 Test ekuivalensi di level protokol (deterministik, host)

Pola sudah ada: `test/panic_test.c:192-233` dan `test/kyuzenfs_xcheck.c:19-40`
mendefinisikan mock `ata_*` sendiri. Untuk redesign ini mock-nya harus naik
kelas: **model device ATA** (`test/ata_devmodel.*`) yang menegakkan aturan
(BSY/DRQ per sektor, `READ SECTORS` count>1, `READ MULTIPLE`, `READ DMA`
opsional, LBA→offset, injeksi ERR/DF/timeout), bukan array byte pasif.

Kasus wajib (jalur lama vs jalur baru, **byte-identik + CRC32 + urutan LBA yang
diminta harus sama**):

| # | Kasus | Yang dijaga |
|---|---|---|
| 1 | 1 sektor (LBA 0) | base case + sektor pertama |
| 2 | 1 blok 4 KB penuh | unit bcache normal |
| 3 | beberapa blok berurutan | DRQ re-assert, tidak ada off-by-one |
| 4 | blok terakhir `disk.img` & area crashdump (8 sektor terakhir) | ujung disk + integritas area panic |
| 5 | rentang lintas batas blok | count>1 dengan lba0 tidak kelipatan 8 |
| 6 | LBA tepat di batas LBA28/akhir disk | tidak membaca melewati disk |
| 7 | Injeksi ERR di tengah transfer | **harus error, bukan nol senyap** |
| 8 | Injeksi DF/timeout | tidak hang; error terdeteksi |

Kasus 7/8 penting secara khusus: hari ini kegagalan jadi "data nol"
(`ata.c:67-71`) dan `ata_read_block4k` selalu `return 0` (`:173`), jadi suite
ini akan **gagal pada kode lama** untuk kasus 7/8 — itu memang tujuannya:
menetapkan kontrak error yang benar sebelum jalur baru ditulis.

### 7.2 Cross-check image nyata (host)

Perluas pola `make test-kyuzenfs-xcheck` (Makefile:322-330: `mkfs.kyuzenfs`
+ image 32 MB + RAM disk di test). Tambahkan: untuk **setiap blok** 0..N-1,
baca dengan jalur lama dan jalur baru → `memcmp` + CRC32 sama; plus akhiri
dengan mount + baca file nyata lewat KFS V4 di image yang sama (bukan cuma
blok mentah). Ini menangkap bug yang lolos dari model §7.1 (mis. salah
interpretasi offset blok vs sektor).

### 7.3 Regresi boot & bring-up (QEMU headless)

Pola harness sudah ada (`test/_ui_probe.py` + `serial.log` dari `make run`):
boot headless, lalu assert dari log serial: (a) mount KyuzenFS sukses,
(b) baris `[CRASHDUMP] area siap di LBA ...` menghitung `total − 8` yang sama,
(c) IDENTIFY mengembalikan jumlah sektor yang sama, (d) satu baca file ujung
ke ujung. Bandingkan log sebelum/sesudah perubahan; **disk.img tidak boleh
dipakai untuk menjalankan apa pun yang menulis** — boot dari salinan.

### 7.4 Benchmark A/B (metodologi sama dengan profiling)

4 sampel ukuran (46 KB / 179 KB / 1,36 MB / 6,88 MB), dingin vs hangat,
interleaved N=5, median + min/max, TSC terkalibrasi. Tanpa ini, "lebih cepat"
tidak bisa diklaim — variansi laporan sebelumnya ±80%.

### 7.5 Keamanan disk (aturan mutlak)

- Semua eksperimen di **salinan** (`test_disk_*.img`); `disk.img` hanya dibaca
  untuk `cp`/`sha256sum`.
- Sebelum & sesudah tiap run: `sha256sum disk.img` harus tetap
  `1426eeb41877...` (§10-D).
- Uji jalur tulis (nanti) hanya pada salinan, lalu `cmp` dua salinan hasil
  urutan tulis yang identik.
- Tidak ada `mkfs` ke `disk.img` asli.

### 7.6 Konkurensi (hanya kalau prasyarat `ata_lock` diambil)

Dua CPU membaca blok berbeda bersamaan dalam loop, lalu verifikasi CRC tiap
buffer. Di README-kan di test: tanpa lock, test ini boleh dinyatakan "expected
fail" sampai `ata_lock` masuk — jangan dibiarkan tanpa catatan.

### 7.7 Jalur panic

`make test-panic` (Makefile:340-346) harus tetap hijau. Tambahkan satu kasus:
crashdump read+write dengan flag jalur baca baru ON **dan** OFF. Crashdump
harus tetap pakai `ata_read_sector` PIO lama — kalau nanti diarahkan ke DMA,
itu keputusan terpisah (butuh bukti DMA aman di konteks panic).

---

## 8. Rencana rollback

1. **Sifat data menjamin rollback read-path aman:** jalur baca tidak pernah
   menulis disk. Rollback perubahan baca **tidak mungkin merusak data**. Yang
   berbahaya hanya jalur tulis (bukan scope dokumen ini) — itulah alasan
   redesign dimulai dari baca.
2. **Satu flag compile-time per stage** (mis. `ATA_READ_BATCH=0/1`,
   `ATA_READ_DMA=0/1`), **kedua jalur selalu dikompilasi**, default = lama.
   Rollback = ubah flag (cepat, tanpa revert commit). Kode lama tetap ada
   sebagai fungsi, bukan `#ifdef` yang mengapusnya.
3. **Satu commit per stage** (bukan satu commit besar), pesan commit menyebut
   opsi mana; revert = satu `git revert` tanpa menarik stage lain.
4. **PIO lama permanen.** `ata_read_sector`/`ata_write_sector` dengan signature
   sekarang dipertahankan sebagai fallback untuk (a) crashdump, (b) device tanpa
   BMDMA, (c) kejadian tak terduga di lapangan.
5. **Titik balik terakhir:** kalau Stage 2 bermasalah setelah merge → matikan
   flag DMA; kalau masih bermasalah → revert commit Stage 2; Stage 1 tetap
   berjalan sendiri.

---

## 9. Pertanyaan terbuka (butuh keputusan Anda sebelum saya menulis kode)

1. **Target:** QEMU/TCG saja, atau hardware asli juga? Ini menentukan apakah
   gain "artefak TCG" (mis. `rep insw`) layak dikejar, atau hanya DMA yang
   relevan.
2. **Prototipe Stage 0:** boleh saya buat branch sekali pakai (instrumentasi
   sementara + qemu headless, semua di salinan disk) dan lapor angkanya,
   tanpa satu pun commit? Ini yang saya minta izinkan pertama.
3. **`rep insw`:** boleh? Butuh helper baru di `include/io.h` + `cld`. Kalau
   Anda ingin nol instruksi asm baru, 1c dibatalkan dan hanya 1a yang jalan.
4. **DMA buffer:** bounce buffer `pmm_alloc_page + memcpy 4 KB` (tanpa API
   baru, ada preseden `gpu_alloc.c`) **atau** tambah `virt_to_phys()` di
   `paging.c` (DMA langsung ke `b->data`)? Saya condong **bounce buffer**
   untuk Stage 2 awal.
5. **Signature error:** boleh ubah `void ata_read_sector` → `int`
   (menyentuh `crashdump.c` + 4 mock test) atau tambah API paralel
   (`ata_read_sector_ex`) dan biarkan API lama? Ini menentukan seberapa
   "murni" commit pertama.
6. **`ata_lock`:** PR terpisah dulu (rekomendasi saya), atau digabung ke
   Stage 1?
7. **LBA48:** primitif baru LBA48-ready sekarang, atau eksplisit LBA28 dulu
   (sesuai 100 MiB) dan disiplin `uint64_t` saja?
8. **Jalur tulis** (flush-per-sektor): ticket setelah jalur baca, atau
   dibarengkan dengan Stage 2 (DMA tulis)?
9. **Kriteria terima Stage 1:** berapa angka yang Anda anggap "cukup" untuk
   lanjut/tidak ke Stage 2? (Usulan saya: Stage 1 diterima bila median baca
   dingin 6,88 MB naik ≥ 1,5×; Stage 2 dipertimbangkan bila < 1,5×.)
10. **Ketergantungan BMDMA:** kalau nanti ada mesin/emulator tanpa bus-master
    DMA, apakah oke bahwa sistem berjalan di PIO (lebih lambat tapi benar) —
    jadi DMA selamanya opsional dengan fallback permanen? (Usulan saya: ya.)

---

## 10. Lampiran

### A. Hitungan op-count (dari kode, bukan asumsi)

Per sektor (`drivers/ata.c:44-78`): 6 `outb` + ≥10 `inb` + 256 `inw` = **272 op**.
Per blok 4 KB: 8 × 272 = **2 176 op**; non-data 128 (**5,9%**), data 2 048 (**94,1%**).
Satu perintah `READ SECTORS` dengan count=8: ~17 non-data + 2 048 data = **2 065 op**
(−5,1% dari 2 176 — sama dengan −111 op non-data).

### B. Implikasi biaya per operasi (§3.2)

| sampel | µs/op tersirat |
|---|---|
| small512 / kyuzen / logo | 1,32 / 1,22 / 1,15 → **seragam ≈ 1,2** |
| big6mb | 3,15 → **outlier 2,6×**, harus diukur ulang interleaved |

Plafon PIO turunan: 2 byte ÷ 1,2 µs = **≈ 1,7 MB/s** (TCG). Cocok dengan
0,57–1,6 MB/s yang terukur.

### C. Perkiraan file yang akan disentuh per stage (rencana, bukan komit)

| Stage | File | Jenis perubahan |
|---|---|---|
| 0 | `drivers/ata.c` (instrumentasi), `kernel/kernel.c` (panggilan), harness QEMU | sementara, di-revert |
| 1 | `drivers/ata.c`, `include/io.h` (+ test baru §7.1/§7.2) | permanen, di balik flag |
| 2 | `drivers/ata.c`, `drivers/pci.c`, `include/pci.h`, (opsional `kernel/paging.c`/`include/paging.h`) | permanen, di balik flag |
| 3 | `kernel/fs/bcache.c`, `drivers/ata.c` (jalur tulis) | ticket terpisah |
| tidak disentuh | `kernel/crashdump.c` (kecuali keputusan §9 #5), `include/ata.h` API lama, `kernel/fs/kfs_*.c`, `disk.img` | — |

### D. Snapshot bukti (saat dokumen ini ditulis)

- `git log -1` = `18389eb`; `git status` bersih kecuali dokumen ini +
  `DOCUMENTATION/design/png-decode-profile.md` (laporan tugas sebelumnya,
  belum di-commit).
- `disk.img` — 104 857 600 byte, sha256
  `1426eeb4187723239875e7f6c5a56861ee50c3ddd99401dc77e762f6f9adcf92`,
  **tidak diubah oleh riset ini** (semua probe hanya read-only, monitor QEMU
  dan `info qtree`, plus salinan sekali pakai yang sudah dibuang).
- QEMU yang dipakai: 10.2.1; device: `piix3-ide` (8086:7010) + `ide-hd`
  (`disk.img`) + `ide-cd` (ISO); **BAR4 I/O ada → BMDMA tersedia**.

### E. Referensi cepat baris kode

| Topik | Lokasi |
|---|---|
| Baca sektor (loop 256 `inw`) | `drivers/ata.c:44-78` (`:75-77`) |
| Tulis sektor + flush per sektor | `drivers/ata.c:80-113` (`:109-112`) |
| Wrapper blok 4 KB (8× baca sektor) | `drivers/ata.c:166-174` |
| Silent-zero pada error | `drivers/ata.c:67-71`; `:173` |
| IDENTIFY + deteksi LBA48 | `drivers/ata.c:124-156` |
| API publik ATA | `include/ata.h:6-14` |
| Helper port I/O (tanpa `insw`) | `include/io.h:1-39` |
| Gate IDT IRQ yang ada | `arch/x86/idt.c:83-87` |
| Remap + mask PIC (IRQ14 tertutup) | `drivers/keyboard.c:63-67` |
| Prinsip crashdump (polling, tanpa IRQ) | `kernel/crashdump.c:9-21`, call site `:75,122,134,237,243` |
| bcache baca 1 blok + lock saat I/O | `kernel/fs/bcache.c:179-184` |
| Model bcache (1 MB, 256 blok) | `include/bcache.h:19-45` |
| Locking FS (order `fs_lock → bcache_lock`) | `kernel/fs/kfs_internal.h:24-32` |
| Heap di luar HHDM | `kernel/heap.c:40-47` |
| Preseden DMA (pmm + hhdm) | `graphics/memory/gpu_alloc.c:9-31` |
| PCI baca saja (tanpa write) | `drivers/pci.c:9-20`, `include/pci.h:18-19` |
| Argumen QEMU (tanpa KVM) | `Makefile:487-494` |
| Target test yang ada | `Makefile:294-346`; `test/panic_test.c:192-233`; `test/kyuzenfs_xcheck.c:19-40` |

---

**Yang saya tidak lakukan (sesuai batasan prompt):** tidak ada kode
implementasi, tidak ada tulisan ke `disk.img`, tidak ada perubahan di
`drivers/`, `kernel/`, atau file lain. Semua instrumen penunjang riset ini
(probe monitor QEMU + salinan disk sekali pakai) sudah dihapus.
