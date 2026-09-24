// libs/text/src/kzraster_ft.c — FreeType raster backend (Phase 2/3/19).
//
// Tanpa KZFONT_USE_FREETYPE: file ini hanya mengekspor kz_ft_backend
// dengan fungsi NULL (raster mati, caller fallback bitmap) — libtext
// tetap link & jalan TANPA FreeType.
//
// Dengan KZFONT_USE_FREETYPE: adapter FT_Memory -> kz_heap_t +
// FT_New_Memory_Face (font_blob_t, tanpa fopen) + grayscale
// FT_RENDER_MODE_NORMAL (coverage 8-bit) + FT bitmap_left/top/advance.
//
// Kegagalan FT (font rusak, glyph missing, OOM) -> return <0, TIDAK
// pernah panic. FT_Face/FT_Library TIDAK PERNAH keluar dari file ini
// (kontrak: FT_* implementation detail — kzfont.h bersih).
//
// SMP (Phase 18, Option A-lite): SATU FT_Library global + face cache
// satu slot (blob terakhir). App Kyuzen hari ini single-threaded per
// task; bila dua task memakai font bersamaan, masing-masing punya
// kz_font_t + heap sendiri, tetapi library global dipakai bergantian —
// untuk tahap ini DOKUMENTASIKAN batasnya: satu rasterize dalam
// penerbangan per proses (tidak ada preempsi di tengah syscall render
// userspace; kernel tidak memakai backend ini). Upgrade path = lock
// di sekitar FT calls bila app multi-threaded tiba.

#include "kzfont.h"

#ifdef KZFONT_USE_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_SYSTEM_H  /* FT_MemoryRec, FT_New_Library */
#include FT_MODULE_H  /* FT_Add_Default_Modules */

// --- FT_Memory -> kz_heap_t (Phase 2) ---
// Kyuzen tidak punya malloc global: userspace = sys_alloc. Pointer heap
// diselipkan di memory->user (kontrak FT_Custom_Memory).
static void *kz_ft_alloc(FT_Memory memory, long size) {
    kz_heap_t *h = (kz_heap_t *)memory->user;
    if (size <= 0) return 0;
    return h->alloc((uint32_t)size);
}

static void kz_ft_free(FT_Memory memory, void *block) {
    kz_heap_t *h = (kz_heap_t *)memory->user;
    if (block) h->free(block);
}

static void *kz_ft_realloc(FT_Memory memory, long cur_size, long new_size,
                           void *block) {
    kz_heap_t *h = (kz_heap_t *)memory->user;
    if (new_size <= 0) {
        if (block) h->free(block);
        return 0;
    }
    if (!block) return h->alloc((uint32_t)new_size);
    if (h->realloc)
        return h->realloc(block, (uint32_t)cur_size, (uint32_t)new_size);
    // Fallback alloc+copy+free (blok FT selalu copyable).
    void *q = h->alloc((uint32_t)new_size);
    if (!q) return 0;
    uint8_t *d = (uint8_t *)q;
    uint8_t *s = (uint8_t *)block;
    long n = cur_size < new_size ? cur_size : new_size;
    for (long i = 0; i < n; i++) d[i] = s[i];
    h->free(block);
    return q;
}

// Library global + face satu slot. Heap milik face = heap font yang
// pertama menginisialisasi slot (diganti saat blob berganti).
static FT_Library g_ft_lib = 0;
static FT_Face g_ft_face = 0;
static const uint8_t *g_ft_blob_data = 0;
static uint32_t g_ft_blob_size = 0;
static kz_heap_t g_ft_heap;
static uint32_t g_ft_px = 0;

static void kz_ft_close_face(void) {
    if (g_ft_face) {
        FT_Done_Face(g_ft_face);
        g_ft_face = 0;
    }
    g_ft_blob_data = 0;
    g_ft_blob_size = 0;
    g_ft_px = 0;
}

// Pastikan library + face untuk blob siap. Return 0 ok, <0 gagal
// (font invalid/corrupt -> caller fallback, bukan panic).
static int kz_ft_ensure(const kz_font_blob_t *blob, const kz_heap_t *heap,
                        uint32_t px) {
    if (!blob || !blob->data || blob->size == 0 || !heap) return -1;
    if (!g_ft_lib) {
        static struct FT_MemoryRec_ memrec;
        // Lifetime: FT_New_Library menyimpan pointer memrec DAN
        // memrec.user melampaui panggilan ini (dipakai setiap FT free).
        // kz_heap_t milik caller (f->heap) mati saat kz_font_destroy —
        // menunjuknya langsung = use-after-free tepat saat ganti font
        // (FT_Done_Face membebaskan face lama via heap yang sudah di-free:
        // PAGE FAULT terobservasi di QEMU). Salin ke static g_ft_heap.
        g_ft_heap = *heap;
        memrec.user = (void *)&g_ft_heap;
        memrec.alloc = kz_ft_alloc;
        memrec.free = kz_ft_free;
        memrec.realloc = kz_ft_realloc;
        if (FT_New_Library(&memrec, &g_ft_lib) != 0) {
            g_ft_lib = 0;
            return -1;
        }
        FT_Add_Default_Modules(g_ft_lib);
    }
    // Segarkan copy (pointer fungsi identik per proses, tapi murah).
    g_ft_heap = *heap;
    if (!g_ft_face || g_ft_blob_data != blob->data ||
        g_ft_blob_size != blob->size) {
        kz_ft_close_face();
        // FT_New_Memory_Face TIDAK men-copy font: blob harus hidup selama
        // face dipakai (kontrak kz_font_blob_t = embedded/borrowed — OK).
        if (FT_New_Memory_Face(g_ft_lib, (const FT_Byte *)blob->data,
                               (FT_Long)blob->size, 0, &g_ft_face) != 0) {
            g_ft_face = 0;
            return -1;
        }
        g_ft_blob_data = blob->data;
        g_ft_blob_size = blob->size;
        g_ft_heap = *heap;
        g_ft_px = 0;
    }
    if (g_ft_px != px) {
        if (FT_Set_Pixel_Sizes(g_ft_face, 0, (FT_UInt)px) != 0) return -1;
        g_ft_px = px;
    }
    return 0;
}

static int kz_ft_rasterize(const kz_font_blob_t *blob, const kz_heap_t *heap,
                           uint32_t codepoint, uint32_t px,
                           kz_glyph_t *meta, uint8_t *buf, uint32_t cap) {
    if (!meta || !buf || px == 0 || px > 256) return -1;
    if (kz_ft_ensure(blob, heap, px) != 0) return -1;
    FT_UInt gindex = FT_Get_Char_Index(g_ft_face, (FT_ULong)codepoint);
    if (gindex == 0) return -1;  // missing glyph -> caller fallback
    if (FT_Load_Glyph(g_ft_face, gindex, FT_LOAD_DEFAULT) != 0) return -1;
    if (FT_Render_Glyph(g_ft_face->glyph, FT_RENDER_MODE_NORMAL) != 0)
        return -1;
    FT_Bitmap *bm = &g_ft_face->glyph->bitmap;
    // Hanya grayscale 8-bit yang didukung tahap ini (FT_PIXEL_MODE_GRAY).
    // Bitmap mono/COLOR lain -> tolak (fallback), bukan konversi diam-diam.
    if (bm->pixel_mode != FT_PIXEL_MODE_GRAY) return -1;
    if (bm->width == 0 || bm->rows == 0 || bm->width > 256 || bm->rows > 256)
        return -1;
    uint32_t need = (uint32_t)bm->width * (uint32_t)bm->rows;
    if (need > cap) return -1;
    for (uint32_t r = 0; r < (uint32_t)bm->rows; r++) {
        uint8_t *src = bm->buffer + r * (uint32_t)bm->pitch;
        uint8_t *dst = buf + r * (uint32_t)bm->width;
        for (uint32_t c = 0; c < (uint32_t)bm->width; c++) dst[c] = src[c];
    }
    meta->left = g_ft_face->glyph->bitmap_left;
    meta->top = g_ft_face->glyph->bitmap_top;
    meta->width = (uint32_t)bm->width;
    meta->height = (uint32_t)bm->rows;
    meta->advance_x = (int32_t)(g_ft_face->glyph->advance.x >> 6);
    meta->advance_y = (int32_t)(g_ft_face->glyph->advance.y >> 6);
    meta->coverage = 0;  // diisi Font Manager dari cache (pitch = width)
    meta->pitch = 0;
    return 0;
}

static int kz_ft_metrics(const kz_font_blob_t *blob, uint32_t px,
                         int *asc, int *desc) {
    // metrics butuh heap utk ensure — pakai heap global slot (face sudah
    // dibuka oleh rasterize sebelumnya; bila belum, tolak). Ini menjaga
    // signature backend tetap stateless-tanpa-heap.
    (void)blob;
    (void)px;
    if (!g_ft_face || !asc || !desc) return -1;
    // Ukuran face mungkin beda dgn px yang diminta -> hanya layani bila
    // cocok (konsistensi measure vs draw; selain itu fallback px).
    if (g_ft_px != px) return -1;
    *asc = (int)(g_ft_face->size->metrics.ascender >> 6);
    *desc = (int)(g_ft_face->size->metrics.descender >> 6);
    return 0;
}

const kz_raster_backend_t kz_ft_backend = { kz_ft_rasterize, kz_ft_metrics };

#else // !KZFONT_USE_FREETYPE

// Tanpa FreeType: backend mati (semua lookup MISS -> fallback bitmap).
// Simbol tetap ada supaya kode app tidak perlu #ifdef.
const kz_raster_backend_t kz_ft_backend = { 0, 0 };

#endif
