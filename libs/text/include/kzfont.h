#ifndef KZFONT_H
#define KZFONT_H

// ============================================================
// libs/text — Kyuzen Font API (public, userspace)
//
// Satu-satunya API font yang boleh dipakai app/libui. FreeType
// (bila diaktifkan) tetap implementation detail di balik
// kz_raster_backend_t — header ini TIDAK mengenal FT_*.
//
// Lapisan:
//   app/libui -> kzfont.h -> Font Manager -> raster backend
//             -> Glyph Cache -> canvas + gui_damage_rect()
//
// Desain: freestanding-C, tanpa alokasi tersembunyi selain lewat
// kz_alloc_fn (default: sys_alloc di app, malloc di host test).
// Thread: satu font dipakai satu task; tidak ada lock internal
// (SMP aman karena tidak ada state global — lihat kz_raster_ft.c).
// ============================================================

#include <stdint.h>
#include <stddef.h>
#include "color_types.h"   // color_t (libs/gui/color, dipakai kernel & app)
#include "color_blend.h"   // color_blend_alpha (blend integer, tanpa float)

#ifdef __cplusplus
extern "C" {
#endif

// --- Allocator injection (Phase 2) ---
// Kyuzen tidak punya satu malloc global: kernel = kmalloc,
// userspace = sys_alloc. Backend tidak boleh mengasumsikan nama.
typedef void *(*kz_alloc_fn)(uint32_t size);
typedef void (*kz_free_fn)(void *ptr);
typedef void *(*kz_realloc_fn)(void *ptr, uint32_t old_size, uint32_t new_size);

typedef struct {
    kz_alloc_fn alloc;
    kz_free_fn free;
    kz_realloc_fn realloc;   // boleh NULL -> fallback alloc+copy+free
} kz_heap_t;

// --- Font blob (Phase 3) ---
// Tahap awal: font EMBEDDED / buffer memori. Tidak ada fopen/VFS di sini.
typedef struct {
    const uint8_t *data;
    uint32_t size;
} kz_font_blob_t;

// --- Glyph coverage 8-bit (Phase 5) ---
// coverage = cakupan grayscale per piksel (0 transparan .. 255 penuh).
// pitch = byte per baris coverage (>= width). Origin: (left, top)
// relatif terhadap pen/baseline (konvensi FreeType bitmap_left/top).
typedef struct {
    int32_t left;       // bearing X
    int32_t top;        // bearing Y (jarak baseline -> puncak bitmap)
    uint32_t width;
    uint32_t height;
    int32_t advance_x;  // pen += advance_x (26.6 -> piksel, sudah dibulatkan)
    int32_t advance_y;
    const uint8_t *coverage;
    uint32_t pitch;
} kz_glyph_t;

// Opaque handle (isi di kzfont.c; ukuran disembunyikan dari caller).
typedef struct kz_font kz_font_t;

// --- Raster backend (vtable, diisi kzraster_ft.c atau mock test) ---
// Backend adalah rasterizer MURNI tanpa state: rasterize() mengisi meta
// (left/top/w/h/advance) + coverage (pitch = width) ke buf.
// Return 0 ok, <0 bila codepoint missing / font rusak / buf kekecilan.
// metrics() mengisi ascender/descender (piksel, >0 / <=0) pada ukuran px.
// Backend TIDAK menyimpan cache — cache milik Font Manager di bawah.
// Backend NULL saat load = rasterisasi mati (lookup selalu MISS, caller
// fallback ke bitmap 8x16 yang sudah ada). Ini membuat libtext aman
// di-link TANPA FreeType.
typedef struct {
    int (*rasterize)(const kz_font_blob_t *blob, const kz_heap_t *heap,
                     uint32_t codepoint, uint32_t px,
                     kz_glyph_t *meta, uint8_t *buf, uint32_t cap);
    int (*metrics)(const kz_font_blob_t *blob, uint32_t px,
                   int *asc, int *desc);
} kz_raster_backend_t;

// --- Font Manager (Phase 4) ---
// backend=NULL -> rasterisasi mati, semua lookup MISS (fallback bitmap
// 8x16 yang sudah ada tetap dipakai caller). Ini membuat libtext aman
// di-link TANPA FreeType.
kz_font_t *kz_font_load(const kz_font_blob_t *blob, const kz_heap_t *heap,
                        const kz_raster_backend_t *backend);
int kz_font_set_size(kz_font_t *font, uint32_t pixel_size);  // 0 ok, <0 gagal
void kz_font_destroy(kz_font_t *font);

// Cache diagnostic (Phase 15): lookups/hits/misses sejak load.
// NULL pointer argumen = field itu dilewati.
void kz_font_stats(kz_font_t *font, uint32_t *lookups, uint32_t *hits,
                   uint32_t *misses);

// Backend FreeType (kzraster_ft.c). Tanpa KZFONT_USE_FREETYPE isinya
// {0,0} (raster mati, aman di-link). Dengan flag itu = raster penuh.
extern const kz_raster_backend_t kz_ft_backend;

// --- UTF-8 (Phase 7) ---
// Decode SATU codepoint dari [p, end). Return byte terpakai (1..4),
// atau 0 bila input tidak valid/habis. Malformed -> U+FFFD, konsumsi
// 1 byte (tidak pernah crash, tidak pernah over-read).
// Dekoder ini dipakai ulang bila repo belum punya (audit: belum ada).
uint32_t kz_utf8_decode(const char *p, const char *end, uint32_t *out_cp);

// --- Layout minimal + metrics (Phase 8/14) ---
// measure: width = total advance, height = ascender-descender pada px
// aktif. Glyph missing di-skip (advance 0) — fallback digambar caller.
int kz_text_measure(kz_font_t *font, const char *text,
                    uint32_t *out_w, uint32_t *out_h);

// --- Rendering (Phase 9/10) ---
// Gambar UTF-8 pada canvas XRGB (stride = piksel per baris) di (x,
// baseline_y). Clipping ke [0,w)x[0,h). Damage: bbox aktual ditulis ke
// *dmg (x,y,w,h) bila dmg != NULL; caller meneruskannya ke
// gui_damage_rect(). Return piksel tersentuh, <0 bila font/param invalid.
int kz_text_draw(uint32_t *canvas, uint32_t cw, uint32_t ch,
                 kz_font_t *font, int x, int baseline_y, color_t color,
                 const char *text, int dmg[4]);

// Replacement box untuk glyph missing (Phase 20): digambar caller bila
// lookup gagal — libtext TIDAK menggambar apa pun untuk codepoint yang
// tidak ada (kebijakan di tangan caller, bukan rasterizer).
#define KZ_REPLACEMENT_CP 0xFFFDu

#ifdef __cplusplus
} // extern "C"
#endif

#endif // KZFONT_H
