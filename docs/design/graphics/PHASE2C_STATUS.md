# Phase 2C.1 + 2C.2 — Command Batching & Fence Async Present: Status

## Ringkasan

Present compositor di backend virtio-gpu sekarang **non-blocking**: setiap
present mengirim `TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH` sebagai **dua
descriptor chain terpisah** dengan **satu `virtq_notify`** (deferred kick) dan
**tanpa busy-poll**. Completion diverifikasi lewat fence di flush berikutnya
(backpressure §9.2). Steady state = 0 busy-poll per frame.

Sekaligus memperbaiki bug di working tree sebelumnya: dua command digabung
dalam SATU chain buffer. Per spec (dan perilaku QEMU), device memproses SATU
`ctrl_hdr` per chain — command kedua dibuang diam-diam → layar freeze di
backend virtio. Batching yang benar: dua chain, satu notify.

## Perubahan

### `drivers/graphics/hw/virtqueue.c/.h` — multi-chain in-flight
- Freelist eksplisit (`free_next[]`) + panjang chain per head (`chain_len[]`)
  — menggantikan model "satu chain aktif, deskriptor berurutan".
- `virtq_submit` boleh dipanggil berkali-kali sebelum notify (deferred kick).
- **`virtq_poll()`** (baru, non-blocking): reap SATU chain selesai dari used
  ring, kembalikan deskriptornya ke freelist — urutan completion bebas.
- `virtq_wait` kini dibangun di atas `virtq_poll` (chain lain yang selesai
  lebih dulu ikut di-reclaim — memperbaiki leak deskriptor pada scan lama).
- Queue controlq diminta 32 (sebelumnya 16) untuk headroom 8 chain in-flight.
- Unit test host-side `tests/host/unit/virtqueue_test.c` + target `make test-virtqueue`
  (item checklist 2A.4): 20/20 PASS — deferred kick, completion out-of-order,
  reclaim antar-chain, daur ulang deskriptor, exhaustion, wait.

### `drivers/graphics/hw/virtio_gpu_dev.c/.h`
- **`virtio_gpu_dev_submit2()`**: dua command → dua chain, flag
  `VIRTIO_GPU_FLAG_FENCE` + fence_id sama, SATU notify, tanpa wait.
  Return fence_id (0 = gagal — caller skip present rect, semantik §8.10).
- Tabel fence per head (`fence_of_head[]`, `last_fence_done`) + response slot
  async per chain di `resp_page` (offset ≥ 512, region sinkron dilindungi).
- `virtio_gpu_dev_poll_fences()` / `fence_done()` / `fence_wait()` (timeout §6.9).
- Jalur sinkron `virtio_gpu_dev_command` kini memakai reap-loop yang sama —
  fence async yang selesai di tengah wait sinkron tetap tercatat.

### `graphics/ghal.h` / `ghal.c` — kontrak fence
- Vtable: `present_fence` / `fence_pending` / `fence_wait` (opsional — WAJIB
  NULL pada backend tanpa `GHAL_CAP_ASYNC_PRESENT`, pola fail-fast cursor ops).
- API: `ghal_present_fence()`, `ghal_fence_pending()`, `ghal_fence_wait()`.

### `graphics/backend/virtio_gpu.c`
- `virtio_present` → submit2 async (tidak ada virtq_wait di present).
- capabilities: `GHAL_CAP_PARTIAL_FLUSH | GHAL_CAP_ASYNC_PRESENT`.

### `kernel/gfx/compositor.c`
- Backpressure §9.2: sebelum upload frame baru, `ghal_fence_wait(fence frame
  sebelumnya)` bila masih pending — backing tidak pernah ditimpa saat DMA
  device masih membacanya. Backend sync: no-op.

## Verifikasi (QEMU 10.2, `-device virtio-gpu-pci,id=vgpu0`, headless + monitor screendump)

| Uji | Hasil |
|---|---|
| `make test-virtqueue` (host) | 20/20 PASS |
| Boot virtio, serial | `[vgpu] device ready`, tanpa error fence/submit |
| Screendump display virtio (1280x800) | desktop tampil, 0% piksel hitam |
| Byte-compare vs baseline committed (metode §8.11) | 0.675% beda — semua di band animasi antar-frame (boot log y=2..79, HUD/tengah y=386..399), area statis identik |
| Fallback tanpa `-device virtio-gpu-pci` | `[vgpu] device not found` → software backend, desktop tampil |

**Catatan pengukuran:** `screendump` monitor TANPA argumen device mengambil
display std-VGA (yang memang tidak dipakai compositor saat backend virtio
aktif) → tampak "hitam". Gunakan `screendump <file> vgpu0` (device perlu
`id=` di command line QEMU). Kondisi ini sama pada build baseline — bukan
regresi Phase 2C.

## Yang belum (jujur)

- Stress test §9.10 (window update cepat, 10 menit) — backpressure belum
  diverifikasi di bawah beban ekstrem.
- Counter `virtq_notify` per frame (§9.6 gpu_diag) belum ada — penghematan
  notify belum diukur angkanya, baru strukturnya.
- Hardware cursor (§9.4), triple buffer (§9.5) — belum dimulai.
- Di `virtio_gpu_dev_submit2`, jalur degradasi head2-gagal praktis mustahil
  (pre-check freelist + single-context) tapi belum pernah tereksekusi di test.
