// libs/text/src/kzfont.c — Font Manager + UTF-8 + cache + layout + draw.
//
// Freestanding: tidak include <string.h>/<stdlib.h> — hanya header Kyuzen.
// Semua alokasi lewat kz_heap_t milik font (sys_alloc di app, malloc di test).
// Tidak ada state global: tiap kz_font_t mandiri (SMP: satu font per task,
// tanpa lock — lihat docs/design/font-rendering.md §thread-safety).

#include "kzfont.h"

// --- tiny helpers (pengganti libc, freestanding-safe) ---
static void kz_memcpy(void *d, const void *s, uint32_t n) {
    uint8_t *dd = (uint8_t *)d;
    const uint8_t *ss = (const uint8_t *)s;
    for (uint32_t i = 0; i < n; i++) dd[i] = ss[i];
}

static void kz_memset(void *d, uint8_t v, uint32_t n) {
    uint8_t *dd = (uint8_t *)d;
    for (uint32_t i = 0; i < n; i++) dd[i] = v;
}

// realloc fallback bila heap.realloc == NULL (ponytail: 5 baris).
// (dipakai evict path bila di masa depan ada resize; saat ini tidak
// dipakai — dihapus bila tetap tak terpakai setelah FT backend stabil.)

// ============================================================
// UTF-8 (Phase 7). Valid, bounds-safe, malformed -> U+FFFD/1 byte.
// Audit: repo belum punya decoder (semua jalur teks memakai `char`
// dan memetakan >127 ke '?' — fb.c, panic_draw.c, libgui.c).
// ============================================================
uint32_t kz_utf8_decode(const char *p, const char *end, uint32_t *out_cp) {
    uint32_t cp = 0xFFFDu;
    uint32_t n = 0;
    if (!p || !end || p >= end) return 0;
    uint8_t c0 = (uint8_t)p[0];
    if (c0 < 0x80) {
        cp = c0;
        n = 1;
    } else if ((c0 & 0xE0) == 0xC0) {
        n = 2;
        cp = c0 & 0x1F;
    } else if ((c0 & 0xF0) == 0xE0) {
        n = 3;
        cp = c0 & 0x0F;
    } else if ((c0 & 0xF8) == 0xF0) {
        n = 4;
        cp = c0 & 0x07;
    } else {
        if (out_cp) *out_cp = 0xFFFDu;  // continuation/stray byte
        return 1;
    }
    if (p + n > end) {  // truncated: jangan over-read
        if (out_cp) *out_cp = 0xFFFDu;
        return 1;
    }
    for (uint32_t i = 1; i < n; i++) {
        uint8_t cc = (uint8_t)p[i];
        if ((cc & 0xC0) != 0x80) {  // bukan continuation -> malformed
            if (out_cp) *out_cp = 0xFFFDu;
            return 1;
        }
        cp = (cp << 6) | (cc & 0x3F);
    }
    // Tolak overlong, surrogate, out-of-range (konversi aman -> FFFD).
    // Hanya untuk n>=2 (1-byte selalu valid di sini).
    if (n >= 2) {
        uint32_t min = n == 2 ? 0x80 : (n == 3 ? 0x800 : 0x10000);
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            cp = 0xFFFDu;
    }
    if (out_cp) *out_cp = cp;
    return n;
}

// ============================================================
// Font Manager + Glyph Cache (Phase 4/6)
//
// Cache: hash open-addressing 256 slot, key (codepoint, px).
// Insert di slot bebas pertama chain; bila penuh, drop seluruh cache
// (full-drop: selalu benar tanpa tombstone; ceiling = miss spike).
// Coverage disimpan di heap block per slot (pitch = width).
// ============================================================
#define KZ_CACHE_SLOTS 256u
#define KZ_MAX_COV_BYTES (256u * 256u)  // tolak glyph absurd (OOB guard)

typedef struct {
    uint8_t used;
    uint32_t codepoint;
    uint32_t px;
    kz_glyph_t glyph;  // coverage menunjuk block heap milik slot
} kz_slot_t;

struct kz_font {
    kz_font_blob_t blob;  // BORROWED (embedded asset milik caller)
    kz_heap_t heap;
    const kz_raster_backend_t *backend;  // boleh NULL (raster mati)
    uint32_t px;                         // ukuran aktif (default 16)
    kz_slot_t *slots;                    // KZ_CACHE_SLOTS (heap)
    uint32_t lookups, hits, misses;
};

kz_font_t *kz_font_load(const kz_font_blob_t *blob, const kz_heap_t *heap,
                        const kz_raster_backend_t *backend) {
    if (!blob || !blob->data || blob->size == 0 || !heap || !heap->alloc ||
        !heap->free)
        return 0;
    kz_font_t *f = (kz_font_t *)heap->alloc(sizeof(kz_font_t));
    if (!f) return 0;
    kz_memset(f, 0, sizeof(*f));
    f->blob = *blob;
    f->heap = *heap;
    f->backend = backend;
    f->px = 16;
    f->slots = (kz_slot_t *)heap->alloc(sizeof(kz_slot_t) * KZ_CACHE_SLOTS);
    if (!f->slots) {
        heap->free(f);
        return 0;
    }
    kz_memset(f->slots, 0, sizeof(kz_slot_t) * KZ_CACHE_SLOTS);
    return f;
}

int kz_font_set_size(kz_font_t *font, uint32_t pixel_size) {
    if (!font || pixel_size == 0 || pixel_size > 256)
        return -1;
    font->px = pixel_size;
    return 0;
}

void kz_font_destroy(kz_font_t *font) {
    if (!font) return;
    if (font->slots) {
        for (uint32_t i = 0; i < KZ_CACHE_SLOTS; i++) {
            if (font->slots[i].used && font->slots[i].glyph.coverage)
                font->heap.free((void *)font->slots[i].glyph.coverage);
        }
        font->heap.free(font->slots);
    }
    font->heap.free(font);
}

void kz_font_stats(kz_font_t *font, uint32_t *lookups, uint32_t *hits,
                   uint32_t *misses) {
    if (!font) return;
    if (lookups) *lookups = font->lookups;
    if (hits) *hits = font->hits;
    if (misses) *misses = font->misses;
}

// Lookup internal: HIT -> *out menunjuk coverage cache (hidup selama
// font hidup). MISS -> rasterize backend + simpan. Return 0 ok,
// <0 missing/gagal (caller SKIP glyph — bukan error fatal).
static int kz_glyph_lookup(kz_font_t *f, uint32_t cp, kz_glyph_t *out) {
    uint32_t h = (cp * 2654435761u) ^ (f->px * 40503u);
    uint32_t idx = h & (KZ_CACHE_SLOTS - 1);
    f->lookups++;
    kz_slot_t *free_slot = 0;
    for (uint32_t i = 0; i < KZ_CACHE_SLOTS; i++) {
        kz_slot_t *s = &f->slots[(idx + i) & (KZ_CACHE_SLOTS - 1)];
        if (!s->used) {
            if (!free_slot) free_slot = s;
            break;  // chain berakhir: kunci pasti belum ada
        }
        if (s->codepoint == cp && s->px == f->px) {
            f->hits++;
            *out = s->glyph;
            return 0;
        }
    }
    f->misses++;
    if (!f->backend || !f->backend->rasterize) return -1;
    // Rasterize ke buffer sementara milik heap, lalu pindahkan ke slot.
    // Dua tahap ini menghindari backend menulis langsung ke cache saat
    // gagal di tengah (slot tidak pernah setengah-terisi).
    kz_glyph_t meta;
    kz_memset(&meta, 0, sizeof(meta));
    // Probe ukuran dulu? Backend menulis coverage langsung; beri buffer
    // heap sementara KZ_MAX_COV_BYTES hanya bila glyph kecil — TIDAK:
    // alokasi sementara penuh boros. Sebagai gantinya backend menjamin
    // menolak glyph > cap (kontrak: buf/cap, return <0 bila kekecilan).
    // Di sini cap = KZ_MAX_COV_BYTES via block sementara sekali pakai.
    // ponytail: sederhanakan — satu block sementara per MISS, di-free
    // setelah copy ke slot (footprint puncak = 1 glyph).
    uint8_t *tmp = (uint8_t *)f->heap.alloc(KZ_MAX_COV_BYTES);
    if (!tmp) return -1;
    int rc = f->backend->rasterize(&f->blob, &f->heap, cp, f->px, &meta, tmp,
                                   KZ_MAX_COV_BYTES);
    if (rc != 0 || meta.width == 0 || meta.height == 0 ||
        meta.width > 256 || meta.height > 256 ||
        meta.width * meta.height > KZ_MAX_COV_BYTES) {
        f->heap.free(tmp);
        return -1;
    }
    uint32_t need = meta.width * meta.height;
    // Insert di slot bebas chain ini. Bila tabel penuh (tak ada slot
    // bebas), drop SELURUH cache + insert ulang — ponytail: evict
    // per-slot merusak probe chain (tombstone), full-drop selalu benar.
    // Ceiling: workload >256 glyph live -> miss spike sesekali.
    kz_slot_t *s = free_slot;
    if (!s) {
        for (uint32_t i = 0; i < KZ_CACHE_SLOTS; i++) {
            if (f->slots[i].used && f->slots[i].glyph.coverage)
                f->heap.free((void *)f->slots[i].glyph.coverage);
        }
        kz_memset(f->slots, 0, sizeof(kz_slot_t) * KZ_CACHE_SLOTS);
        s = &f->slots[idx];
    }
    if (s->used && s->glyph.coverage) f->heap.free((void *)s->glyph.coverage);
    uint8_t *cov = (uint8_t *)f->heap.alloc(need);
    if (!cov) {
        s->used = 0;  // slot dikosongkan, bukan dibiarkan basi
        s->glyph.coverage = 0;
        f->heap.free(tmp);
        return -1;
    }
    kz_memcpy(cov, tmp, need);
    f->heap.free(tmp);
    s->used = 1;
    s->codepoint = cp;
    s->px = f->px;
    s->glyph = meta;
    s->glyph.coverage = cov;
    s->glyph.pitch = meta.width;
    *out = s->glyph;
    return 0;
}

// ============================================================
// Measure (Phase 8/14): width = total advance glyph ADA;
// height = ascender - descender pada px aktif.
// Missing glyph: advance 0 (fallback digambar caller).
// ============================================================
int kz_text_measure(kz_font_t *font, const char *text,
                    uint32_t *out_w, uint32_t *out_h) {
    if (!font || !text || !out_w || !out_h) return -1;
    uint32_t w = 0;
    const char *end = text;
    while (*end) end++;  // NUL-terminated (API app-level; kernel pakai bounded)
    const char *p = text;
    while (p < end) {
        uint32_t cp = 0;
        uint32_t n = kz_utf8_decode(p, end, &cp);
        if (n == 0) break;
        p += n;
        kz_glyph_t g;
        if (kz_glyph_lookup(font, cp, &g) == 0 && g.advance_x > 0)
            w += (uint32_t)g.advance_x;
    }
    int asc = (int)(font->px), desc = -(int)(font->px / 4);
    if (font->backend && font->backend->metrics) {
        int a = 0, d = 0;
        if (font->backend->metrics(&font->blob, font->px, &a, &d) == 0) {
            asc = a;
            desc = d;
        }
    }
    int h = asc - desc;
    if (h <= 0) h = (int)font->px;
    *out_w = w;
    *out_h = (uint32_t)h;
    return 0;
}

// ============================================================
// Draw (Phase 9/10): coverage + warna -> alpha blend -> canvas.
// coverage 0 = skip, 255 = warna penuh (color_blend_alpha).
// Canvas XRGB: byte alpha = mask opaque (kontrak display.h) — dst
// dibaca opaque sebelum blend (pola sama dgn Painter::blend).
// Clipping ke canvas; dmg[4] = bbox aktual (x,y,w,h) utk
// gui_damage_rect() — TANPA damage subsystem baru.
// ============================================================
int kz_text_draw(uint32_t *canvas, uint32_t cw, uint32_t ch,
                 kz_font_t *font, int x, int baseline_y, color_t color,
                 const char *text, int dmg[4]) {
    if (!canvas || cw == 0 || ch == 0 || !font || !text) return -1;
    const char *end = text;
    while (*end) end++;
    int pen = x;
    int touched = 0;
    int x0 = x, y0 = baseline_y, x1 = x, y1 = baseline_y;
    int any = 0;
    const char *p = text;
    while (p < end) {
        uint32_t cp = 0;
        uint32_t n = kz_utf8_decode(p, end, &cp);
        if (n == 0) break;
        p += n;
        kz_glyph_t g;
        if (kz_glyph_lookup(font, cp, &g) != 0) continue;  // fallback caller
        int gx = pen + g.left;
        int gy = baseline_y - g.top;
        for (uint32_t r = 0; r < g.height; r++) {
            int py = gy + (int)r;
            if (py < 0 || py >= (int)ch) continue;
            for (uint32_t c = 0; c < g.width; c++) {
                int px = gx + (int)c;
                if (px < 0 || px >= (int)cw) continue;
                uint32_t cov = g.coverage[r * g.pitch + c];
                if (cov == 0) continue;
                // Alpha efektif = coverage x alpha warna (integer).
                uint32_t a = (cov * (uint32_t)color.a + 127u) / 255u;
                if (a == 0) continue;
                uint32_t *dst = &canvas[(uint32_t)py * cw + (uint32_t)px];
                color_t src = color_with_alpha(color, (uint8_t)a);
                color_t dcol =
                    color_opaque(color_from_u32(*dst, FORMAT_ARGB));
                *dst = color_to_u32(color_blend_alpha(src, dcol),
                                    FORMAT_ARGB);
                if (!any) {
                    x0 = x1 = px;
                    y0 = y1 = py;
                    any = 1;
                } else {
                    if (px < x0) x0 = px;
                    if (px > x1) x1 = px;
                    if (py < y0) y0 = py;
                    if (py > y1) y1 = py;
                }
                touched++;
            }
        }
        if (g.advance_x > 0) pen += g.advance_x;
    }
    if (dmg) {
        if (any) {
            dmg[0] = x0;
            dmg[1] = y0;
            dmg[2] = x1 - x0 + 1;
            dmg[3] = y1 - y0 + 1;
        } else {
            dmg[0] = dmg[1] = dmg[2] = dmg[3] = 0;
        }
    }
    return touched;
}
