// tests/host/unit/text_test.c — host test libs/text (tanpa QEMU/FT).
//
// Pola test-color/test-pipe: kode PRODUKSI (libs/text/src/kzfont.c)
// dikompilasi apa adanya; raster backend = MOCK deterministik (bukan
// salinan logika). Yang dikunci:
//   1. UTF-8: 1..4 byte valid, malformed->FFFD, truncated aman, bounds.
//   2. Manager: validasi param, set_size, destroy, OOM-inject graceful.
//   3. Cache: render 2x -> MISS lalu HIT (Phase 15).
//   4. Measure == lebar aktual render (Phase 14, toleransi bearing).
//   5. Blend: cov 0 tak menyentuh, 255 = warna penuh, tengah = campur.
//   6. Damage: bbox == area piksel tersentuh; clip tepi aman.
//   7. Missing glyph di-skip (advance 0), invalid font -> -1 (no crash).
//
// Build: make test-text (root Makefile).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kzfont.h"

// ---------- heap host (malloc) + OOM injection ----------
static int g_fail_after = -1;  // >=0: alloc gagal setelah N sukses
static int g_alloc_ok = 0;

static void *t_alloc(uint32_t n) {
    if (g_fail_after >= 0 && g_alloc_ok >= g_fail_after) return 0;
    g_alloc_ok++;
    return malloc(n ? n : 1);
}

static void t_free(void *p) { free(p); }

static void *t_realloc(void *p, uint32_t o, uint32_t n) {
    (void)o;
    if (n == 0) {
        free(p);
        return 0;
    }
    return realloc(p, n);
}

static kz_heap_t t_heap(void) {
    kz_heap_t h;
    h.alloc = t_alloc;
    h.free = t_free;
    h.realloc = t_realloc;
    return h;
}

// ---------- mock raster: box deterministik ----------
// w = 6 + cp%5, h = px, adv = w+2, solid 255 kecuali kolom 0 = 128.
static int mock_raster(const kz_font_blob_t *blob, const kz_heap_t *heap,
                       uint32_t cp, uint32_t px, kz_glyph_t *meta,
                       uint8_t *buf, uint32_t cap) {
    (void)blob;
    (void)heap;
    if (cp == 0x10FFFFu) return -1;  // simulasi missing glyph
    uint32_t w = 6 + cp % 5;
    if (w * px > cap) return -1;
    meta->left = 1;
    meta->top = (int32_t)(px * 3 / 4);
    meta->width = w;
    meta->height = px;
    meta->advance_x = (int32_t)(w + 2);
    meta->advance_y = 0;
    meta->coverage = 0;
    meta->pitch = 0;
    for (uint32_t r = 0; r < px; r++)
        for (uint32_t c = 0; c < w; c++)
            buf[r * w + c] = c == 0 ? 128 : 255;
    return 0;
}

static int mock_metrics(const kz_font_blob_t *blob, uint32_t px, int *a,
                        int *d) {
    (void)blob;
    *a = (int)px;
    *d = -(int)(px / 4);
    return 0;
}

static const kz_raster_backend_t MOCK = { mock_raster, mock_metrics };
static const uint8_t FAKEBLOB[64] = { 1 };

// ---------- mini framework ----------
static int g_fail = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            g_fail++;                                                   \
        }                                                               \
    } while (0)

static kz_font_t *load_mock(uint32_t px) {
    kz_heap_t h = t_heap();
    kz_font_blob_t b = { FAKEBLOB, sizeof(FAKEBLOB) };
    kz_font_t *f = kz_font_load(&b, &h, &MOCK);
    if (f) kz_font_set_size(f, px);
    return f;
}

int main(void) {
    // --- 1. UTF-8 ---
    {
        uint32_t cp = 0;
#define DEC(s) kz_utf8_decode(s, s + sizeof(s) - 1, &cp)
        static const char t_ascii[] = "A";
        static const char t_eacute[] = "\xC3\xA9";
        static const char t_euro[] = "\xE2\x82\xAC";
        static const char t_emoji[] = "\xF0\x9F\x98\x80";
        static const char t_stray[] = "\xFF";
        static const char t_trunc[] = "\xE2\x82";
        static const char t_overlong[] = "\xC0\xAF";
        static const char t_surrog[] = "\xED\xA0\x80";
        CHECK(DEC(t_ascii) == 1 && cp == 0x41);
        CHECK(DEC(t_eacute) == 2 && cp == 0xE9);      // e-acute
        CHECK(DEC(t_euro) == 3 && cp == 0x20AC);      // euro
        CHECK(DEC(t_emoji) == 4 && cp == 0x1F600);    // emoji
        CHECK(DEC(t_stray) == 1 && cp == 0xFFFD);     // stray byte
        CHECK(DEC(t_trunc) == 1 && cp == 0xFFFD);     // truncated
        CHECK(DEC(t_overlong) == 2 && cp == 0xFFFD);  // overlong
        CHECK(DEC(t_surrog) == 3 && cp == 0xFFFD);    // surrogate
#undef DEC
        static const char t_empty[] = "";
        CHECK(kz_utf8_decode(t_empty, t_empty, &cp) == 0);  // empty bounds
        CHECK(kz_utf8_decode(0, t_empty, &cp) == 0);        // NULL input
        CHECK(kz_utf8_decode(t_ascii, t_ascii + 1, 0) == 1);  // NULL out
    }

    // --- 2. Manager validation + OOM ---
    {
        kz_heap_t h = t_heap();
        kz_font_blob_t b = { FAKEBLOB, sizeof(FAKEBLOB) };
        kz_font_blob_t bad = { 0, 0 };
        CHECK(kz_font_load(0, &h, &MOCK) == 0);
        CHECK(kz_font_load(&bad, &h, &MOCK) == 0);
        CHECK(kz_font_load(&b, 0, &MOCK) == 0);
        kz_font_t *f = kz_font_load(&b, &h, &MOCK);
        CHECK(f != 0);
        CHECK(kz_font_set_size(f, 0) == -1);
        CHECK(kz_font_set_size(f, 999) == -1);
        CHECK(kz_font_set_size(0, 16) == -1);
        CHECK(kz_font_set_size(f, 20) == 0);
        kz_font_destroy(f);
        kz_font_destroy(0);  // no crash
        g_fail_after = 0;
        g_alloc_ok = 0;
        CHECK(kz_font_load(&b, &h, &MOCK) == 0);  // OOM -> NULL, no crash
        g_fail_after = -1;
    }

    // --- 3. Cache: miss lalu hit ---
    {
        kz_font_t *f = load_mock(16);
        uint32_t l0, h0, m0, l1, h1, m1;
        static uint32_t cv[320 * 200];
        color_t white = COLOR_RGB(0xFF, 0xFF, 0xFF);
        int dmg[4];
        kz_text_draw(cv, 320, 200, f, 8, 40, white, "Hello Kyuzen", dmg);
        kz_font_stats(f, &l0, &h0, &m0);
        // 12 codepoint, 10 unik ('l'/'e' berulang -> 2 hit di draw pertama).
        CHECK(l0 == 12 && m0 == 10 && h0 == 2);
        kz_text_draw(cv, 320, 200, f, 8, 40, white, "Hello Kyuzen", dmg);
        kz_font_stats(f, &l1, &h1, &m1);
        CHECK(m1 == m0);           // tidak ada miss baru
        CHECK(h1 > h0);            // mostly hits
        CHECK(l1 == l0 + 12);      // 12 codepoint diulang
        kz_font_destroy(f);
    }

    // --- 4. Measure == render aktual ---
    {
        kz_font_t *f = load_mock(16);
        uint32_t w, h;
        CHECK(kz_text_measure(f, "Hello Kyuzen", &w, &h) == 0);
        CHECK(h == 16 + 4);  // asc 16, desc -4
        // Lebar harapan: sum(6 + cp%5 + 2) per char.
        uint32_t exp = 0;
        const char *s = "Hello Kyuzen";
        for (const char *p = s; *p; p++) exp += 6 + (uint32_t)*p % 5 + 2;
        CHECK(w == exp);
        static uint32_t cv[320 * 200];
        memset(cv, 0, sizeof(cv));
        color_t white = COLOR_RGB(0xFF, 0xFF, 0xFF);
        int dmg[4];
        kz_text_draw(cv, 320, 200, f, 10, 60, white, s, dmg);
        // bbox kanan-kiri <= advance total (bearing kanan), > 0.
        CHECK(dmg[2] > 0 && (uint32_t)dmg[2] <= w);
        CHECK(kz_text_measure(0, s, &w, &h) == -1);
        kz_font_destroy(f);
    }

    // --- 5. Blend: 0 skip, 255 penuh, 128 campur ---
    {
        kz_font_t *f = load_mock(8);
        static uint32_t cv[64 * 32];
        for (int i = 0; i < 64 * 32; i++) cv[i] = 0xFF000000u;  // hitam
        color_t white = COLOR_RGB(0xFF, 0xFF, 0xFF);
        int dmg[4];
        // 'A' (0x41): w = 6+65%5 = 6, kolom 0 cov=128, sisanya 255.
        kz_text_draw(cv, 64, 32, f, 0, 16, white, "A", dmg);
        // gx = 0+left(1) = 1, gy = 16 - top(6) = 10.
        uint32_t p_half = cv[10 * 64 + 1];  // kolom 0 -> a=128
        uint32_t p_full = cv[10 * 64 + 2];  // kolom 1 -> a=255
        CHECK(p_full == 0xFFFFFFFFu);
        // 128/255 putih di atas hitam -> ~0x808080 (toleransi +-2).
        uint32_t r = (p_half >> 16) & 0xFF;
        CHECK(r >= 126 && r <= 130);
        CHECK((p_half & 0xFF) >= 126 && (p_half & 0xFF) <= 130);
        kz_font_destroy(f);
    }

    // --- 6. Damage + clip tepi ---
    {
        kz_font_t *f = load_mock(16);
        static uint32_t cv[32 * 32];
        memset(cv, 0, sizeof(cv));
        color_t white = COLOR_RGB(0xFF, 0xFF, 0xFF);
        int dmg[4] = { 9, 9, 9, 9 };
        int t = kz_text_draw(cv, 32, 32, f, -100, -100, white, "Hi", dmg);
        CHECK(t == 0);  // sepenuhnya di luar -> tak tersentuh
        CHECK(dmg[2] == 0 && dmg[3] == 0);
        t = kz_text_draw(cv, 32, 32, f, 0, 16, white, "", dmg);
        CHECK(t == 0);  // string kosong
        kz_font_destroy(f);
    }

    // --- 7. Missing glyph + invalid font + backend mati ---
    {
        kz_font_t *f = load_mock(16);
        static uint32_t cv[64 * 32];
        memset(cv, 0, sizeof(cv));
        color_t white = COLOR_RGB(0xFF, 0xFF, 0xFF);
        int dmg[4];
        // "\xF4\x8F\xBF\xBF" = U+10FFFF -> mock return -1 (missing).
        int t = kz_text_draw(cv, 64, 32, f, 0, 16, white, "A\xF4\x8F\xBF\xBF"
                                                          "B",
                             dmg);
        CHECK(t > 0);  // A dan B tetap ter-render, missing di-skip
        CHECK(kz_text_draw(0, 64, 32, f, 0, 16, white, "A", dmg) == -1);
        CHECK(kz_text_draw(cv, 64, 32, 0, 0, 16, white, "A", dmg) == -1);
        kz_heap_t h = t_heap();
        kz_font_blob_t b = { FAKEBLOB, sizeof(FAKEBLOB) };
        kz_font_t *nb = kz_font_load(&b, &h, 0);  // backend mati
        CHECK(nb != 0);
        uint32_t w, hh;
        CHECK(kz_text_measure(nb, "Hi", &w, &hh) == 0);
        CHECK(w == 0);  // tanpa raster: advance 0, bukan crash
        CHECK(kz_ft_backend.rasterize == 0);  // tanpa FT flag: mati
        kz_font_destroy(nb);
        kz_font_destroy(f);
    }

    if (g_fail == 0) printf("text_test: ALL PASS\n");
    return g_fail != 0;
}
