// tests/host/unit/text_ft_real.c — real FreeType raster via kzfont API.
//
// BUKAN mock: memuat assets/fonts/DejaVuSans.ttf (Bitstream Vera
// license, lihat DejaVuSans-LICENSE.txt) lewat kz_ft_backend
// (KZFONT_USE_FREETYPE) + port freestanding kzf_port.c. Membuktikan:
//   DejaVu TTF -> blob -> FT_Face -> 16px -> 'A' -> coverage 8-bit
//   -> kz_glyph_t -> cache -> canvas.
//
// Dijalankan: make test-text-ft (dari root repo; path font relatif).
// fopen HANYA di test host ini (memuat aset uji) — libtext sendiri
// tetap tanpa filesystem (blob borrowed).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kzfont.h"

#define FONT_PATH "assets/fonts/DejaVuSans.ttf"

static int g_fail = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            g_fail++;                                                   \
        }                                                               \
    } while (0)

static void *t_alloc(uint32_t n) { return malloc(n ? n : 1); }
static void t_free(void *p) { free(p); }
static void *t_realloc(void *p, uint32_t o, uint32_t n) {
    (void)o;
    if (n == 0) {
        free(p);
        return 0;
    }
    return realloc(p, n);
}

int main(void) {
    FILE *fp = fopen(FONT_PATH, "rb");
    CHECK(fp != 0);
    if (!fp) return 1;
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    CHECK(fsize > 100000 && fsize < 5000000);
    uint8_t *data = (uint8_t *)malloc((size_t)fsize);
    CHECK(data != 0);
    CHECK(fread(data, 1, (size_t)fsize, fp) == (size_t)fsize);
    fclose(fp);

    kz_heap_t heap = { t_alloc, t_free, t_realloc };
    kz_font_blob_t blob = { data, (uint32_t)fsize };
    kz_font_t *f = kz_font_load(&blob, &heap, &kz_ft_backend);
    CHECK(f != 0);
    CHECK(kz_font_set_size(f, 16) == 0);

    // --- measure 'A' ---
    uint32_t w = 0, h = 0;
    CHECK(kz_text_measure(f, "A", &w, &h) == 0);
    printf("glyph 'A' 16px: measure w=%u h=%u\n", w, h);
    CHECK(w > 0 && h > 0);

    // --- draw 'A', periksa piksel aktual ---
    static uint32_t cv[128 * 64];
    memset(cv, 0, sizeof(cv));
    color_t white = COLOR_RGB(0xFF, 0xFF, 0xFF);
    int dmg[4] = { 0, 0, 0, 0 };
    int touched = kz_text_draw(cv, 128, 64, f, 8, 48, white, "A", dmg);
    printf("draw 'A': touched=%d dmg=(%d,%d,%d,%d)\n", touched, dmg[0],
           dmg[1], dmg[2], dmg[3]);
    CHECK(touched > 0);
    CHECK(dmg[2] > 0 && dmg[3] > 0);
    CHECK((uint32_t)dmg[2] <= w);  // bbox dalam advance (bearing kanan)

    // --- grayscale nyata: harus ada tepi anti-alias (0 < v < 255) ---
    // (piksel canvas di atas latar hitam: v==255 putih penuh, 0<v<255 AA)
    int full = 0, edge = 0;
    for (int i = 0; i < 128 * 64; i++) {
        uint32_t r = (cv[i] >> 16) & 0xFFu;
        if (r == 255) full++;
        else if (r != 0) edge++;
    }
    printf("pixels: full=%d edge(AA)=%d\n", full, edge);
    CHECK(full > 0);  // inti glyph opaque
    CHECK(edge > 0);  // tepi anti-alias 8-bit (bukan bitmap 1-bit)

    // --- cache: draw kedua harus HIT semua (tanpa raster ulang) ---
    uint32_t l0, h0, m0, l1, h1, m1;
    kz_font_stats(f, &l0, &h0, &m0);
    CHECK(m0 == 1);  // 'A' diraster SEKALI (measure+draw share cache)
    memset(cv, 0, sizeof(cv));
    kz_text_draw(cv, 128, 64, f, 8, 48, white, "A", dmg);
    kz_font_stats(f, &l1, &h1, &m1);
    CHECK(m1 == m0 && h1 > h0);

    // --- string metavariable: advances > 0 semua ---
    uint32_t w2, h2;
    CHECK(kz_text_measure(f, "Hello Kyuzen", &w2, &h2) == 0);
    printf("measure \"Hello Kyuzen\" 16px: w=%u h=%u\n", w2, h2);
    CHECK(w2 > w && h2 == h);

    // --- blob rusak: gagal graceful (bukan crash) ---
    {
        static const uint8_t junk[64] = { 0xDE, 0xAD, 0xBE, 0xEF };
        kz_font_blob_t bad = { junk, sizeof(junk) };
        kz_font_t *fb = kz_font_load(&bad, &heap, &kz_ft_backend);
        CHECK(fb != 0);  // load ok (blob borrowed, belum di-parse)
        CHECK(kz_font_set_size(fb, 16) == 0);
        uint32_t bw = 999, bh = 999;
        CHECK(kz_text_measure(fb, "A", &bw, &bh) == 0);
        CHECK(bw == 0);  // raster gagal -> advance 0, bukan crash
        memset(cv, 0, sizeof(cv));
        CHECK(kz_text_draw(cv, 128, 64, fb, 8, 48, white, "A", dmg) == 0);
        kz_font_destroy(fb);
    }

    kz_font_destroy(f);
    free(data);

    if (g_fail == 0) printf("text_ft_real: ALL PASS\n");
    return g_fail != 0;
}
