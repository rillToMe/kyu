# KyuzenOS Graphics — API Reference

Referensi lengkap API Graphics subsystem. Semua tipe & fungsi publik berada di
`include/graphics/` (dimirror ke `driver/graphics/` untuk konsumen driver).

Dokumen ini dibagi per layer:
1. Surface (`surface.h`)
2. HAL (`gpu.h`)
3. Surface Manager (`surface_manager.h`)
4. Renderer / 2D API (`graphics.h`)

---

## 1. Surface (`graphics/surface.h`)

### Tipe

```c
typedef enum {
    GPU_PIXEL_XRGB8888 = 0,   // 32bpp, byte atas = mask opaque/alpha
    GPU_PIXEL_ARGB8888 = 1,   // 32bpp, alpha 8-bit di byte atas
    GPU_PIXEL_RGB565   = 2,   // 16bpp
} gpu_pixel_format_t;

typedef struct { int32_t x, y; uint32_t width, height; } gpu_rect_t;

typedef enum { GPU_MEM_NONE, GPU_MEM_SYSTEM, GPU_MEM_VRAM } gpu_mem_t;
typedef enum {
    GPU_SURFACE_FRONT, GPU_SURFACE_BACK, GPU_SURFACE_WINDOW,
    GPU_SURFACE_OFFSCREEN, GPU_SURFACE_TEXTURE,
} gpu_surface_kind_t;
```

### `gpu_surface_t`

```c
typedef struct gpu_surface {
    uint32_t          width;       // lebar (piksel)
    uint32_t          height;      // tinggi (piksel)
    uint32_t          stride;      // piksel per baris (>= width)
    gpu_pixel_format_t format;
    gpu_mem_t         memory;
    gpu_surface_kind_t kind;
    uint32_t          flags;
    uint32_t          handle;      // dari surface manager (0 = tidak dikelola)
    uint32_t          refcount;
    uint32_t          vram_offset; // offset di heap VRAM (0 = n/a)
    void*             pixels;      // alamat CPU (software / shared)
    uint8_t           owns_pixels; // 1 = manager bebas-kan pixels
} gpu_surface_t;
```

### Helper (implementasi `graphics/hal/surface.c`)

```c
// Clip `r` ke batas surface. Return 1 jika ada area tersisa (out ditulis).
int  gpu_surface_clip(const gpu_surface_t* s, gpu_rect_t* r);
// Pointer baris ke-`row` (0..height-1), NULL bila invalid.
uint32_t* gpu_surface_row(const gpu_surface_t* s, uint32_t row);
// Byte per piksel format surface.
int  gpu_surface_bpp(const gpu_surface_t* s);
```

---

## 2. Graphics HAL (`graphics/gpu.h`)

### Hasil operasi

```c
typedef enum {
    GPU_OK = 0,
    GPU_ERR_UNSUPPORTED = -1,
    GPU_ERR_NOMEM       = -2,
    GPU_ERR_INVALID_ARG = -3,
    GPU_ERR_NO_GPU      = -4,
    GPU_ERR_BUSY        = -5,
} gpu_result_t;
```

### Flag blit

```c
#define GPU_BLEND_NONE   0x00   // overwrite
#define GPU_BLEND_ALPHA  0x01   // source-over alpha
#define GPU_BLEND_KEY    0x02   // color-key (byte atas 0 = skip)
```

### `gpu_backend_t` & `gpu_backend_ops_t`

Struktur backend + vtable ops (lihat `gpu.h` untuk definisi penuh). Setiap
backend mengisi setiap member ops; ops yang belum didukung boleh me-return
`GPU_ERR_UNSUPPORTED`.

```c
struct gpu_backend {
    const char*               name;
    const gpu_backend_ops_t*  ops;
    void*                     priv;
    gpu_surface_t             front;     // buffer depan aktif
    uint8_t                   initialized;
};
```

### Dispatch

```c
gpu_result_t gpu_register_backend(gpu_backend_t* backend); // daftarkan backend
gpu_result_t gpu_initialize(void);                          // probe + aktifkan yang pertama sukses
gpu_backend_t* gpu_active(void);                            // backend aktif (NULL bila tak ada)
void          gpu_shutdown(void);                           // shutdown semua backend

// Wrapper NULL-safe ke backend aktif (return GPU_ERR_NO_GPU bila tak ada):
gpu_result_t gpu_create_surface(gpu_surface_t* s);
void         gpu_destroy_surface(gpu_surface_t* s);
gpu_result_t gpu_upload_texture(gpu_surface_t* dst, const void* src, size_t src_stride);
gpu_result_t gpu_fill_rect(gpu_surface_t* dst, const gpu_rect_t* r, uint32_t color);
gpu_result_t gpu_draw_line(gpu_surface_t* dst, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color);
gpu_result_t gpu_draw_image(gpu_surface_t* dst, int32_t dx, int32_t dy, const gpu_surface_t* src, const gpu_rect_t* src_rect);
gpu_result_t gpu_blit(gpu_surface_t* dst, int32_t dx, int32_t dy, const gpu_surface_t* src, const gpu_rect_t* src_rect, uint32_t flags);
gpu_result_t gpu_stretch_blit(gpu_surface_t* dst, const gpu_rect_t* dr, const gpu_surface_t* src, const gpu_rect_t* sr, uint32_t flags);
gpu_result_t gpu_alpha_blend(gpu_surface_t* dst, const gpu_rect_t* dr, const gpu_surface_t* src, const gpu_rect_t* sr);
gpu_result_t gpu_present(const gpu_rect_t* damage, uint32_t damage_count);
gpu_result_t gpu_flush(void);
gpu_result_t gpu_wait_idle(void);
```

---

## 3. Surface Manager (`graphics/surface_manager.h`)

```c
#define GPU_MAX_SURFACES 64

void     gpu_surface_manager_init(void);

// Buat surface yang di-backing RAM sistem. Return handle (>=1) atau 0.
uint32_t gpu_surface_create(uint32_t width, uint32_t height,
                            gpu_pixel_format_t format, gpu_surface_kind_t kind);

// Buat surface membungkus memori milik caller (borrowed, tak di-free).
uint32_t gpu_surface_wrap(void* pixels, uint32_t width, uint32_t height,
                          uint32_t stride, gpu_pixel_format_t format,
                          gpu_surface_kind_t kind);

uint32_t gpu_surface_retain(uint32_t handle);   // naikkan refcount
void     gpu_surface_release(uint32_t handle);  // turunkan; hancurkan bila 0

gpu_surface_t* gpu_surface_get(uint32_t handle); // lookup (jangan di-free oleh caller)
void           gpu_surface_foreach(void (*cb)(const gpu_surface_t*, void*), void* ctx);

// GPU memory manager (VRAM khusus):
uint32_t gpu_vram_alloc(uint32_t size, uint32_t align);
void     gpu_vram_free(uint32_t offset, uint32_t size);
// Statistik VRAM (total/used/peak/count). Pointer boleh NULL.
void     gpu_vram_stats(uint32_t* bytes_total, uint32_t* bytes_used,
                        uint32_t* bytes_peak, uint32_t* allocation_count);
```

Catatan kepemilikan:
- `gpu_surface_create` → manager **memiliki** pixel backing (di-free di release terakhir).
- `gpu_surface_wrap` → manager **meminjam** (borrowed), `owns_pixels = 0`, tak pernah di-free.

---

## 4. Renderer / 2D API (`graphics/graphics.h`)

```c
gpu_result_t gfx_graphics_init(void);     // daftar backend + init HAL + buat front/back

// Inisialisasi dengan front buffer milik caller (mis. framebuffer hardware).
// stride_bytes = stride baris front buffer dalam byte; front_pixels NULL →
// front buffer privat dibuat.
gpu_result_t gfx_graphics_init_with(void* front_pixels, uint32_t width,
                                    uint32_t height, uint32_t stride_bytes);

gfx_canvas_t* gfx_canvas_acquire(void);   // dapatkan render target (back buffer)
void          gfx_canvas_release(gfx_canvas_t* c);

// Primitif (semua menandai canvas dirty):
void gfx_fill_rect(gfx_canvas_t* c, const gpu_rect_t* r, uint32_t color);
void gfx_draw_line(gfx_canvas_t* c, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color);
void gfx_draw_image(gfx_canvas_t* c, int32_t dx, int32_t dy, const gpu_surface_t* src, const gpu_rect_t* src_rect);
void gfx_blit(gfx_canvas_t* c, int32_t dx, int32_t dy, const gpu_surface_t* src, const gpu_rect_t* src_rect, uint32_t flags);
void gfx_stretch_blit(gfx_canvas_t* c, const gpu_rect_t* dr, const gpu_surface_t* src, const gpu_rect_t* sr, uint32_t flags);

// Present: `full=1` → seluruh surface; selain itu hanya region damage.
void gfx_present(gfx_canvas_t* c, int full);
void gfx_clear_damage(gfx_canvas_t* c);

// Bridge present untuk kompositor: copy damage back→front mentah lewat HAL.
gpu_result_t gfx_compositor_present(void* back_pixels, uint32_t back_stride,
                                    void* front_pixels, uint32_t front_stride,
                                    uint32_t width, uint32_t height,
                                    const gpu_rect_t* damage, uint32_t damage_count);

void gfx_graphics_shutdown(void);
```

`gfx_canvas_t`:

```c
typedef struct gfx_canvas {
    gpu_surface_t* surface;   // surface back buffer
    gpu_rect_t     damage;    // bounding box kumulatif region yang berubah
    uint32_t       dirty;     // nonzero = damage valid
} gfx_canvas_t;
```

---

## 5. Contoh penggunaan minimal

```c
#include "graphics/graphics.h"

void contoh(void) {
    if (gfx_graphics_init() != GPU_OK) return;

    gfx_canvas_t* c = gfx_canvas_acquire();
    if (!c) return;

    gpu_rect_t r = { 10, 10, 100, 50 };
    gfx_fill_rect(c, &r, 0x00FF00);     // kotak hijau
    gfx_draw_line(c, 0, 0, 200, 200, 0xFF0000);  // garis merah diagonal

    gfx_present(c, 0);                  // present region damage saja
    gfx_clear_damage(c);

    gfx_canvas_release(c);
    gfx_graphics_shutdown();
}
```
