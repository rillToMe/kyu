# KyuzenOS Graphics — Documentation

Dokumentasi subsistem **Hardware Accelerated 2D Graphics** KyuzenOS.

## Dokumen

| Dokumen | Isi |
|---------|-----|
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | Arsitektur layer, source tree, HAL GPU, surface, double buffering, konvensi desain |
| [`API.md`](API.md) | Referensi lengkap API HAL / surface / surface manager / renderer + contoh |
| [`BACKEND_GUIDE.md`](BACKEND_GUIDE.md) | Panduan menambah backend GPU baru (VirtIO, SVGA, Bochs, Intel, AMD, NVIDIA) |
| [`PHASE1_5_REPORT.md`](PHASE1_5_REPORT.md) | Laporan selesai Phase 1.5 (fondasi software) |
| [`PHASE2A_STATUS.md`](PHASE2A_STATUS.md) | Status Phase 2A: VirtIO-GPU bring-up (PCI→MMIO→virtqueue→scanout→present) |
| [`PHASE2B_STATUS.md`](PHASE2B_STATUS.md) | Status Phase 2B: compositor present lewat HAL, desktop identik di 2 backend |

## Konvensi

- Ditulis dalam Bahasa Indonesia (mengikuti konvensi `DOCUMENTATION/`),
  identifier, path, dan command dibiarkan apa adanya.
- Satu topik = satu file `.md`.

## Peta singkat

- **HAL** `graphics/hal/gpu.c` → `include/graphics/gpu.h`
- **Surface** `graphics/hal/surface.c` → `include/graphics/surface.h`
- **Surface manager** `graphics/surface/surface_manager.c` → `include/graphics/surface_manager.h`
- **Renderer** `graphics/renderer/graphics.c` → `include/graphics/graphics.h`
- **Software backend** `graphics/backend/software/software_gpu.c`
- **Build** `graphics/CMakeLists.txt` (opsional; kernel utama pakai Makefile)

Header publik juga dimirror ke `driver/graphics/`.
