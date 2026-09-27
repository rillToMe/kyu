# Phase 2A — VirtIO GPU Bring-up: Status

## Ringkasan

Driver VirtIO-GPU diimplementasikan end-to-end dan terverifikasi di QEMU:
pipeline **PCI → MMIO → device-status FSM → feature negotiation → virtqueue →
resource → scanout → present** berfungsi penuh.

### Verifikasi di QEMU (GFX_SELFTEST, `-device virtio-gpu-pci`)

```
[vgpu] device ready
[vgpu] scanout 1280x800
[GFX SELFTEST] PASS (backend=virtio-gpu)
[GFX SELFTEST] present OK
```

Fallback software (tanpa `-device virtio-gpu-pci`):

```
[vgpu] device not found
[GFX SELFTEST] PASS (backend=software)
[GFX SELFTEST] present OK
```

## File baru

```
drivers/graphics/hw/virtio_gpu_regs.h     register & command layout (packed)
drivers/graphics/hw/virtqueue.h/.c        split virtqueue generik
drivers/graphics/hw/virtio_gpu_dev.h/.c   PCI + MMIO + FSM + negotiation + command
drivers/graphics/hw/virtio_gpu_cmd.h/.c   command encoder
graphics/memory/gpu_alloc.h/.c            alokasi physical pages (backing)
graphics/ghal.h/.c                        Graphics HAL (dispatch + select)
graphics/backend/software.c               software backend (fallback)
graphics/backend/virtio_gpu.c             VirtIO-GPU backend
```

## Keputusan implementasi

- **PCI capability layout**: `virtio_pci_cap` struct punya `padding[3]` setelah
  `bar`; `offset` di +8, `length` di +12, `notify_off_multiplier` di +16.
  Bug awal (offset +5/+9/+13) diperbaiki setelah debug.
- **PCI byte read**: `pci_read_word` hanya benar untuk offset even. Tambah
  `pci_cfg_read8` (baca word di `offset&~1`, pilih byte via `offset&1`).
- **MMIO via HHDM**: BAR di-map lewat `hhdm_offset` (Limine map seluruh RAM);
  akses lewat pointer `volatile`.
- **Command buffer multi-page**: `ATTACH_BACKING` untuk surface besar punya
  command > 1 halaman. `virtio_gpu_dev_command` mengalokasikan `ceil(cmd_len/4096)`
  halaman dan mengirim sebagai descriptor chain.
- **Format**: XRGB8888 kita = `VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM` (byte B,G,R,X).
- **Virtqueue v1 synchronous**: submit→notify→busy-poll used ring dengan
  `VIRTQ_POLL_MAX_ITER` timeout (pola §6.9).

## Yang belum (jujur)

- Unit test host-side `tests/host/unit/virtqueue_test.c` & `tests/host/unit/virtio_gpu_cmd_test.c`
  (virtqueue & command sudah jalan di QEMU asli; test mock belum ditulis).
- Verifikasi **visual** warna solid di layar QEMU: memerlukan virtio-gpu sebagai
  primary display (`-vga none`), yang berkonflik dengan boot Limine (butuh VGA
  framebuffer). Command pipeline sudah dibuktikan bekerja; menampilkan di layar
  diselesaikan saat migrasi compositor (Phase 2B) memakai virtio-gpu sebagai
  scanout aktif.

## Lanjut Phase 2B

Migrasi compositor (`kernel/gfx/compositor.c`) untuk memakai `ghal_*` API:
setiap `kwm_window_t` punya `ghal_surface_t`, `sys_kwm_update_window` memanggil
`ghal_surface_upload`, dan `composite_windows_in_rect` memanggil `ghal_blit` +
`ghal_present`. (Perlu resolusi scanout diakses compositor — tambah
`ghal_scanout_size()` atau ekspos dari backend.)
