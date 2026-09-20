# Kyuzen OS — Documentation

Dokumentasi Kyuzen OS diorganisir per topik (meniru gaya `Documentation/` di
Linux), menggantikan satu file raksasa secara bertahap.

> Dokumen lama: [`../DOCUMENTATION.md`](../DOCUMENTATION.md) masih memuat
> dokumentasi Task & Multitasking — akan dimigrasikan ke folder ini.

## Daftar Isi

| Folder | Isi |
|--------|-----|
| [`design/`](design/) | Dokumen desain/arsitektur: keputusan, invarian, constraint ABI, dan verifikasinya |
| [`troubleshooting/`](troubleshooting/) | Post-mortem insiden/bug: gejala, investigasi, root cause, fix, dan teknik debugging yang bisa dipakai ulang |

### design/

| Dokumen | Ringkasan |
|---------|-----------|
| [`ring3-tahap1-cpl3.md`](design/ring3-tahap1-cpl3.md) | Ring 3 Tahap 1 — ELF apps jalan di CPL 3 via TSS per-CPU + syscall stack permanen; syscall tetap `int 0x80` |
| [`ring3-tahap2-boundary-copy.md`](design/ring3-tahap2-boundary-copy.md) | Ring 3 Tahap 2 — boundary copy: pointer user divalidasi + di-copy in/out via `kernel/usercopy.c`; kontrak per-syscall (copy/shared/bypass); fix UAF `sys_load_elf` |
| [`sched-module-split.md`](design/sched-module-split.md) | Pemecahan `kernel/task.c` (907 baris) jadi modul `kernel/sched/` (core, runqueue, lifecycle, block, debug) — tanpa perubahan perilaku, `task.h` tidak berubah |
| [`fs-phase1-3-directories.md`](design/fs-phase1-3-directories.md) | KyuzenFS 3 fase: folder sebagai entry biasa + path absolut, syscall path (`sys_get_file_list`/`sys_mkdir`), migrasi app ke `/apps/` |
| [`color-library.md`](design/color-library.md) | Library warna modular `libs/color/`: `color_t` RGBA, blending integer, HSL/HSV, palet + utility UI; zero-alloc, kernel & user-space, verifikasi sweep 256³ warna |
| [`gui-phase11-image-viewer.md`](design/gui-phase11-image-viewer.md) | Image Viewer: sidebar + auto-fit (tanpa geser manual) + statusbar; API libui baru `ui_image_set_fit`/`ui_image_natural_size`/`ui_scrollview_set_pan`/`ui_listview_set_selected` |
| [`widget-split.md`](design/widget-split.md) | Pemecahan toolkit widget `apps/libui.cpp` (3.798 baris) jadi `libs/widget/` per-layer (`core`/`primitives`/`editor`/`layout`/`containers`/`chrome`/`dialog`/`window`/`services` + `abi`) — tanpa perubahan perilaku; aturan dependency antar layer, peta file lama→baru, wiring build, dan hasil verifikasi (ABI publik 115/115 identik) |

### troubleshooting/

| Dokumen | Ringkasan |
|---------|-----------|
| [`2026-07-26-heap-corruption-bosd.md`](troubleshooting/2026-07-26-heap-corruption-bosd.md) | BOSD "heap_block_t magic mismatch" saat buka PNG — ternyata bukan corruptor, melainkan halaman heap terpetakan ke ROM BIOS karena free list PMM tercemar mapping Limine |

### graphics/ — Hardware Accelerated 2D Graphics

Subsistem 2D Graphics berada di [`../graphics/docs/`](../graphics/docs/),
mendokumentasikan HAL GPU, surface, dan panduan backend:

| Dokumen | Ringkasan |
|---------|-----------|
| [`../graphics/docs/ARCHITECTURE.md`](../graphics/docs/ARCHITECTURE.md) | Arsitektur layer, source tree, HAL, surface, double buffering, konvensi |
| [`../graphics/docs/API.md`](../graphics/docs/API.md) | Referensi lengkap API HAL / surface / surface manager / renderer + contoh |
| [`../graphics/docs/BACKEND_GUIDE.md`](../graphics/docs/BACKEND_GUIDE.md) | Panduan menambah backend GPU baru (VirtIO, SVGA, Bochs, Intel, AMD, NVIDIA) |

---

## Konvensi

- Satu topik = satu folder; satu insiden/topik = satu file `.md`.
- Nama file post-mortem: `YYYY-MM-DD-<judul-singkat>.md`.
- Tulis dalam Bahasa Indonesia; biarkan identifier, path, command, dan log
  apa adanya (tidak diterjemahkan).
- Sertakan bukti mentah (potongan log, alamat, disassembly) — bukan hanya
  kesimpulan — supaya pembaca bisa memverifikasi ulang.
