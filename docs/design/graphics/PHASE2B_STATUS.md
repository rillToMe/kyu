# Phase 2B — Hardware Rendering: Status

## Ringkasan

Compositor KyuzenOS sekarang **present sepenuhnya lewat Graphics HAL** (`ghal_*`).
Backend dipilih otomatis saat boot: **virtio-gpu** bila device ada, **software**
sebagai fallback. Output visual identik di kedua backend.

## Verifikasi

**Screendump 1280x800, dibandingkan byte-per-byte:**

| Metrik | Hasil |
|---|---|
| Total byte dibandingkan | 3.072.000 |
| Byte berbeda | 14.343 (**0.467%**) |
| Lokasi perbedaan | **Hanya** area yang memang berubah antar-frame |

Semua perbedaan berada di:
- HUD spinner (update tiap 200 ms)
- HUD uptime (update tiap 1 s)
- TTY boot log yang masih scroll

Area statis (wallpaper/desktop) **identik byte-per-byte**. Ini bukan regresi
rendering — dua screendump diambil pada waktu berbeda, jadi elemen animasi wajib
berbeda.

**Log boot (virtio-gpu):**
```
[vgpu] device ready
[GHAL] backend=virtio-gpu
[GFX SELFTEST] backend=virtio-gpu scanout=1280x800
```

**Log boot (fallback, tanpa `-device virtio-gpu-pci`):**
```
[vgpu] device not found
[GHAL] backend=software
[GFX SELFTEST] backend=software scanout=1280x800
```

## Perubahan

### `kernel/gfx/compositor.c`
- `compositor_flush()` **tidak lagi menulis `fb_ptr`**. Langkah terakhir
  (backbuffer → layar) sekarang: `ghal_surface_upload` per dirty-rect →
  **satu** `ghal_present` dengan bounding rect gabungan (batching §8.9).
- `compositor_ghal_init()` membuat main surface **di konteks task** (dipanggil
  dari `kernel_main`), bukan lazy di IRQ — `surface_create` memakai `kmalloc`
  dan (pada virtio) virtqueue command, keduanya tidak aman di IRQ handler.
- Fallback ke jalur langsung (`blit_rect_db` ke `fb_db`) bila main surface gagal.

### `graphics/ghal.h` / `ghal.c`
- `ghal_set_framebuffer()` — beri tahu software backend lokasi framebuffer HW.
- `ghal_scanout_size()` — resolusi output backend aktif.

### `graphics/backend/software.c`
- `present()` sekarang **benar-benar menyalin** region damage dari surface ke
  framebuffer hardware (sebelumnya no-op).

### `graphics/backend/virtio_gpu.c`
- `SET_SCANOUT` dipindah ke `present()` (sekali per surface, flag `scanout_set`)
  supaya main surface compositor benar-benar jadi output aktif.
- **Format diperbaiki**: `B8G8R8X8_UNORM`, bukan `X8R8G8B8_UNORM`. Nama format
  virtio menyatakan urutan **BYTE** di memori. XRGB8888 kita (`0x00RRGGBB`
  little-endian) tersusun B,G,R,X. Memakai X8R8G8B8 membuat device membaca byte0
  sebagai X dan byte3 sebagai B → **channel biru hilang** (terbukti: abu-abu
  `30,30,30` tampil `30,30,0`).

### `drivers/graphics/hw/virtio_gpu_dev.c/.h`
- Buffer command/response **pre-alokasi sekali saat probe**, dipakai ulang dan
  dilindungi `cmd_lock`. Sebelumnya tiap command memanggil `pmm_alloc_page` —
  tidak aman & lambat di IRQ (`compositor_flush` berjalan di timer IRQ).
- Copy command **per halaman** (halaman tidak contiguous), bukan satu `memcpy`
  besar ke `cmd_pages[0]` yang akan meluber ke memori lain.

## Yang ditunda (jujur)

- **Per-window `ghal_surface_t`** (roadmap §8.1/§8.8): window canvas tetap
  `DisplayBuffer` system-memory; compositor menyusun frame di `backbuffer` lalu
  upload satu main surface. Output identik dan jauh lebih sederhana. Per-window
  surface baru bermanfaat saat blit dilakukan GPU-side — pada VirtIO-GPU 2D
  fill/blit tetap CPU-side (roadmap §8.3 mengakui ini eksplisit), jadi
  memecah ke N surface hanya menambah command tanpa keuntungan.
- **5 skenario compositing terisolasi** (single/overlap/drag/resize/close)
  belum diotomasi; verifikasi dilakukan pada desktop boot penuh.
- Unit test host-side `virtqueue_test.c` / `virtio_gpu_cmd_test.c` (dari 2A).

## Catatan performa (roadmap §8.3)

Manfaat Phase 2B adalah **arsitektural**, bukan speedup mentah: fill/blit tetap
CPU-side pada VirtIO-GPU 2D. Yang didapat: present terpisah dari raw framebuffer
write, dan compositor siap untuk backend hardware-accelerated sungguhnya tanpa
perubahan kontrak.
