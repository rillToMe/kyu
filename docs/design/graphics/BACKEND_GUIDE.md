# KyuzenOS Graphics — Backend Guide

Panduan untuk menambah **backend GPU baru** ke Graphics HAL. Backend dipilih
otomatis saat boot: `gpu_initialize()` mengaktifkan backend pertama yang
berhasil `initialize()`. Prioritas = urutan `gpu_register_backend()`.

---

## 1. Alur hidup backend

```
boot
 ├─ gfx_graphics_init()
 │    ├─ software_gpu_register()      # daftar backend software (prioritas rendah)
 │    ├─ <new>_gpu_register()         # daftar backend baru (prioritas tinggi)
 │    └─ gpu_initialize()             # probe tiap backend, aktifkan yang pertama sukses
 ├─ gpu_create_surface / gpu_*        # semua panggilan di-routing ke backend aktif
 └─ gpu_shutdown()                    # shutdown semua backend terdaftar
```

**Aturan prioritas:** daftar backend yang lebih cepat di-`register` mendapat
prioritas lebih tinggi. Urutan yang disarankan:

```
software → virtio-gpu → svga → bochs → intel → amd → nvidia
```

Dengan urutan ini, sistem selalu punya fallback software bila tidak ada
hardware yang dikenali, tanpa mengubah satu baris pun di layer atas.

---

## 2. Langkah-langkah menambah backend

### 2.1 Buat file

```
graphics/backend/<nama>/<nama>_gpu.c
graphics/backend/<nama>/<nama>_gpu.h
```

Contoh nama: `virtio_gpu`, `svga`, `bochs_gfx`, `intel_gfx`, `amd_gfx`,
`nvidia_gfx`.

### 2.2 Implementasikan `gpu_backend_ops_t`

Salin kontrak dari `graphics/backend/software/software_gpu.c`. Setiap ops:

| Ops               | Wajib? | Catatan                                                |
|-------------------|--------|--------------------------------------------------------|
| `initialize`      | ya     | deteksi hardware; return `GPU_OK` bila dikenali        |
| `shutdown`        | ya     | lepaskan resource / reset device                       |
| `create_surface`  | ya     | siapkan surface di device (bisa no-op untuk shared)    |
| `destroy_surface` | ya     | balikkan resource device                               |
| `upload_texture`  | opsional | `GPU_ERR_UNSUPPORTED` bila tak ada                      |
| `fill_rect`       | ya     |                                                        |
| `draw_line`       | opsional | boleh fallback ke software                             |
| `draw_image`      | ya     |                                                        |
| `blit`            | ya     |                                                        |
| `stretch_blit`    | opsional |                                                        |
| `alpha_blend`     | opsional |                                                        |
| `present`         | ya     | submit frame / swap buffer                             |
| `flush`           | opsional | dorong command queue ke device                         |
| `wait_idle`       | ya     | sinkron: tunggu GPU selesai semua kerja                |

Ops yang tidak didukung **wajib** return `GPU_ERR_UNSUPPORTED`, bukan
dibiarkan `NULL` (dispatch akan jatuh ke error yang sama).

### 2.3 Ekspos singleton + registrasi

```c
// <nama>_gpu.h
#include "graphics/gpu.h"
extern gpu_backend_t g_<nama>_backend;
gpu_result_t <nama>_gpu_register(void);
```

```c
// <nama>_gpu.c
static const gpu_backend_ops_t g_<nama>_ops = { ... };
gpu_backend_t g_<nama>_backend = {
    .name = "<nama>",
    .ops  = &g_<nama>_ops,
    .priv = ...,
};
gpu_result_t <nama>_gpu_register(void) {
    return gpu_register_backend(&g_<nama>_backend);
}
```

### 2.4 Integrasi build

**Makefile** (`SRC_DIRS`):

```
graphics/backend/<nama>
```

**CMakeLists.txt** (`graphics/CMakeLists.txt` → `GRAPHICS_SOURCES`):

```
backend/<nama>/<nama>_gpu.c
```

### 2.5 Daftarkan sebelum `gpu_initialize()`

Panggil `<nama>_gpu_register()` dari `gfx_graphics_init()` (renderer) **sebelum**
`gpu_initialize()`:

```c
gpu_result_t gfx_graphics_init(void) {
    software_gpu_register();       // fallback terakhir
    <nama>_gpu_register();         // prioritas lebih tinggi
    return gpu_initialize();
}
```

---

## 3. Kontrak visual

Setiap backend **harus** menghasilkan output yang sama dengan software backend
untuk input yang sama. Ini menjamin aplikasi tidak peduli backend mana yang
aktif.

Konvensi pixel (XRGB8888):
- 24 bit rendah = `0xRRGGBB`.
- Byte atas = mask opaque/alpha: `0x00` = transparan, `0xFF` = opaque.
- `GPU_BLEND_KEY`: piksel dengan byte atas `0x00` dilewati (tidak ditulis).
- `GPU_BLEND_ALPHA`: source-over blending (`src` di atas `dst`).

---

## 4. Memori

### Shared memory backend (VirtIO, Bochs)

Set `s.memory = GPU_MEM_SYSTEM` dan `s.pixels` menunjuk buffer yang bisa
di-akses CPU. `upload_texture` = `memcpy`. `present` = kirim buffer/offset ke
device.

### Dedicated VRAM backend (SVGA, Intel, AMD, NVIDIA)

1. Alokasi offset VRAM via `gpu_vram_alloc(size, align)`.
2. Simpan offset di `s.vram_offset`; set `s.memory = GPU_MEM_VRAM`.
3. Untuk akses CPU, map offset ke RAM (BAR) atau gunakan
   `upload_texture`/`download_texture` (tambahkan `download_texture` ke
   `gpu_backend_ops_t` bila perlu).
4. `gpu_vram_free(offset, size)` saat surface di-destroy (manager tidak
   otomatis memanggilnya — lakukan di `destroy_surface`).

---

## 5. Sinkronisasi

- Backend bertanggung jawab atas keamanan SMP-nya sendiri.
- Software backend memakai spinlock per-instance.
- Backend hardware harus mengawal command queue / ring buffer dengan lock yang
  sesuai (lihat pola `net_lock` pada e1000: satu lock men-serialkan semua entry
  ke lwIP — prinsip yang sama berlaku di sini).
- `wait_idle()` harus benar-benar menunggu device selesai sebelum
  `gpu_present()` berikutnya membaca/menulis buffer.

---

## 6. Checklist

- [ ] `initialize()` mendeteksi hardware dan return `GPU_OK` hanya bila benar ada.
- [ ] Semua ops vtable terisi; yang tak didukung return `GPU_ERR_UNSUPPORTED`.
- [ ] Output visual identik dengan software backend.
- [ ] Backend di-`register` sebelum `gpu_initialize()`.
- [ ] `SRC_DIRS` + CMake diperbarui.
- [ ] Backend tidak membocorkan register/MMIO ke layer atas.
- [ ] Ops tak mengalokasi di hot path; alokasi di init/create.
