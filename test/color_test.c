// Host-side unit test: libs/color — tipe, blending, ruang warna, utility UI.
// Integer murni, tanpa kernel/QEMU (ala test/aa_math_test.c).
//
// Build: clang -Ilibs/color/include test/color_test.c libs/color/src/*.c -o test/color_test
// Run:   ./test/color_test  (atau: make test-color)
//
// Gagal = blending/konversi warna yang dipakai compositor, toolkit, dan app
// rusak. Cakupan: serialisasi 4 format framebuffer, round-trip u32, blend
// (ujung/tengah/span), HSL & HSV (primer + round-trip), darken/lighten,
// auto-contrast, dan nilai palet.

#include <assert.h>
#include <stdio.h>

#include "aa_math.h"   // pembanding paritas color_blend_alpha (jalur lama compositor)
#include "color_blend.h"
#include "color_space.h"
#include "color_types.h"
#include "color_utils.h"

static int color_eq(color_t a, color_t b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

static int channel_diff(int a, int b) {
    int d = a - b;
    return d < 0 ? -d : d;
}

static int max3(int a, int b, int c) {
    int m = a > b ? a : b;
    return m > c ? m : c;
}

// Batas galat round-trip RGB <-> HSL/HSV dari kuantisasi H (1 derajat) +
// S/L/V (8-bit). Sweep seluruh 256^3 warna di bawah membuktikan angka ini.
#define ROUNDTRIP_TOL 3

static int color_close(color_t a, color_t b, int tol) {
    return channel_diff(a.r, b.r) <= tol && channel_diff(a.g, b.g) <= tol &&
           channel_diff(a.b, b.b) <= tol && channel_diff(a.a, b.a) <= tol;
}

// 0x00RRGGBB — enak dibandingkan dengan literal hex gaya lama.
static uint32_t rgb24(color_t c) {
    return ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
}

static void test_types(void) {
    color_t c = COLOR_RGBA(0x11, 0x22, 0x33, 0x44);
    assert(c.r == 0x11 && c.g == 0x22 && c.b == 0x33 && c.a == 0x44);
    assert(COLOR_RGB(1, 2, 3).a == 255);

    // Serialisasi: byte memori little-endian per format.
    assert(color_to_u32(c, FORMAT_ARGB) == 0x44112233u);
    assert(color_to_u32(c, FORMAT_RGBA) == 0x11223344u);
    assert(color_to_u32(c, FORMAT_ABGR) == 0x44332211u);
    assert(color_to_u32(c, FORMAT_BGRA) == 0x33221144u);

    // Round-trip to_u32 -> from_u32 untuk semua format.
    const color_format_t fmts[] = { FORMAT_ARGB, FORMAT_RGBA, FORMAT_ABGR, FORMAT_BGRA };
    for (int i = 0; i < 4; i++) {
        color_t back = color_from_u32(color_to_u32(c, fmts[i]), fmts[i]);
        assert(color_eq(back, c));
    }

    // Decode hex gaya display KyuzenOS (0xAARRGGBB), mis. ps1_color libui.
    color_t hex = color_from_u32(0xFF7CC7FFu, FORMAT_ARGB);
    assert(hex.r == 0x7C && hex.g == 0xC7 && hex.b == 0xFF && hex.a == 0xFF);

    // Helper nilai: ganti cakupan / paksa opaque tanpa mengubah komponen lain.
    color_t faded = color_with_alpha(c, 7);
    assert(faded.r == c.r && faded.g == c.g && faded.b == c.b && faded.a == 7);
    assert(color_opaque(COLOR_TRANSPARENT).a == 255);
    assert(color_opaque(COLOR_TRANSPARENT).r == 0);
}

static void test_blend(void) {
    color_t dst = COLOR_BLUE;

    // Ujung: alpha 0 tidak menggambar, alpha 255 menutup penuh.
    assert(color_eq(color_blend_alpha(COLOR_RGBA(255, 0, 0, 0), dst), dst));
    assert(color_eq(color_blend_alpha(COLOR_RED, dst), COLOR_RED));

    // Latar transparan → src menang; hasil selalu opaque.
    color_t over_empty = color_blend_alpha(COLOR_RGBA(10, 20, 30, 128), COLOR_TRANSPARENT);
    assert(color_eq(over_empty, COLOR_RGB(10, 20, 30)));

    // 50%: putih di atas hitam = abu 128, alpha hasil 255.
    color_t mid = color_blend_alpha(COLOR_RGBA(255, 255, 255, 128), COLOR_BLACK);
    assert(mid.r == 128 && mid.g == 128 && mid.b == 128 && mid.a == 255);

    // color_div255 harus persis x/255 untuk seluruh rentang produk kanal.
    for (uint32_t x = 0; x <= 255u * 255u; x++) {
        assert(color_div255(x) == x / 255u);
    }

    // Span batch = hasil per-pixel.
    color_t span_dst[3] = { COLOR_BLACK, COLOR_BLUE, COLOR_WHITE };
    color_t span_expect[3] = { span_dst[0], span_dst[1], span_dst[2] };
    const color_t span_src[3] = {
        COLOR_RGBA(255, 255, 255, 64),
        COLOR_RGBA(0, 0, 0, 32),
        COLOR_RGBA(255, 0, 0, 200),
    };
    for (int i = 0; i < 3; i++) {
        span_expect[i] = color_blend_alpha(span_src[i], span_expect[i]);
    }
    color_blend_span(span_dst, span_src, 3);
    for (int i = 0; i < 3; i++) {
        assert(color_eq(span_dst[i], span_expect[i]));
    }
}

// Paritas dengan aa_mix (jalur blend lama compositor/libui): wajib identik
// pixel-per-pixel di atas canvas opaque, supaya migrasi tidak mengubah render.
static void test_aa_mix_parity(void) {
    const uint32_t srcs[4] = { 0x123456, 0xFFFFFF, 0x000000, 0xE81123 };
    const uint32_t dsts[4] = { 0x000000, 0xFFFFFF, 0x808080, 0x1E293B };
    for (int s = 0; s < 4; s++) {
        for (int d = 0; d < 4; d++) {
            for (uint32_t a = 0; a <= 255; a++) {
                color_t src = color_from_u32(srcs[s], FORMAT_ARGB);
                color_t dst = color_from_u32(dsts[d], FORMAT_ARGB);
                src.a = (uint8_t)a;
                dst.a = 255;   // canvas compositor diperlakukan opaque
                uint32_t got = color_to_u32(color_blend_alpha(src, dst), FORMAT_ARGB) & 0xFFFFFFu;
                assert(got == aa_mix(srcs[s], dsts[d], a));
            }
        }
    }
}

static void test_hsl(void) {
    // Hue primer & sekunder.
    assert(color_rgb_to_hsl(COLOR_RED).h == 0);
    assert(color_rgb_to_hsl(COLOR_GREEN).h == 120);
    assert(color_rgb_to_hsl(COLOR_BLUE).h == 240);
    assert(color_rgb_to_hsl(COLOR_YELLOW).h == 60);
    assert(color_rgb_to_hsl(COLOR_CYAN).h == 180);
    assert(color_rgb_to_hsl(COLOR_MAGENTA).h == 300);

    // Abu-abu tanpa saturasi; hitam/putih di ujung lightness.
    assert(color_rgb_to_hsl(COLOR_GRAY).s == 0);
    assert(color_rgb_to_hsl(COLOR_BLACK).l == 0);
    assert(color_rgb_to_hsl(COLOR_WHITE).l == 255);
    assert(color_rgb_to_hsl(COLOR_RED).s == 255);
    assert(color_rgb_to_hsl(COLOR_RED).l == 128);

    // s=0 → abu-abu, alpha hasil selalu 255.
    color_t gray = color_hsl_to_rgb((color_hsl_t){ 210, 0, 64 });
    assert(gray.r == 64 && gray.g == 64 && gray.b == 64 && gray.a == 255);

    // Round-trip semua nilai uji (toleransi kuantisasi 8-bit).
    const color_t samples[] = {
        COLOR_BLACK, COLOR_WHITE, COLOR_RED, COLOR_GREEN, COLOR_BLUE,
        COLOR_YELLOW, COLOR_CYAN, COLOR_MAGENTA, COLOR_GRAY,
        { 100, 150, 200, 255 }, { 17, 200, 90, 255 }, { 250, 5, 130, 255 },
    };
    for (unsigned i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        color_t back = color_hsl_to_rgb(color_rgb_to_hsl(samples[i]));
        assert(color_close(back, samples[i], ROUNDTRIP_TOL));
        assert(back.a == 255);
    }
}

static void test_hsv(void) {
    // Primer & sekunder: hue sama dengan HSL, value = kanal max.
    assert(color_rgb_to_hsv(COLOR_RED).h == 0);
    assert(color_rgb_to_hsv(COLOR_GREEN).h == 120);
    assert(color_rgb_to_hsv(COLOR_BLUE).h == 240);
    assert(color_rgb_to_hsv(COLOR_RED).s == 255 && color_rgb_to_hsv(COLOR_RED).v == 255);
    assert(color_rgb_to_hsv(COLOR_BLACK).v == 0 && color_rgb_to_hsv(COLOR_BLACK).s == 0);
    assert(color_rgb_to_hsv(COLOR_WHITE).s == 0 && color_rgb_to_hsv(COLOR_WHITE).v == 255);

    // s=0 → abu-abu; alpha hasil selalu 255.
    color_t gray = color_hsv_to_rgb((color_hsv_t){ 0, 0, 200 });
    assert(gray.r == 200 && gray.g == 200 && gray.b == 200 && gray.a == 255);

    // Enam warna primer/sekunder round-trip eksak; sample lain bertoleransi.
    const color_t exact[] = { COLOR_RED, COLOR_GREEN, COLOR_BLUE,
                              COLOR_YELLOW, COLOR_CYAN, COLOR_MAGENTA };
    for (unsigned i = 0; i < sizeof(exact) / sizeof(exact[0]); i++) {
        assert(color_eq(color_hsv_to_rgb(color_rgb_to_hsv(exact[i])), exact[i]));
    }
    const color_t samples[] = {
        COLOR_BLACK, COLOR_WHITE, COLOR_GRAY,
        { 100, 150, 200, 255 }, { 17, 200, 90, 255 }, { 250, 5, 130, 255 },
    };
    for (unsigned i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        color_t back = color_hsv_to_rgb(color_rgb_to_hsv(samples[i]));
        assert(color_close(back, samples[i], ROUNDTRIP_TOL));
    }

    // Arbitrary hue: 0..359 tidak boleh keluar jalur (wrap aman).
    for (unsigned h = 0; h < 360; h++) {
        color_t c = color_hsv_to_rgb((color_hsv_t){ (uint16_t)h, 255, 255 });
        assert(c.a == 255);
    }
}

// Sweep penuh: bukti batas galat round-trip untuk seluruh ruang RGB.
static void test_roundtrip_sweep(void) {
    int worst_hsl = 0;
    int worst_hsv = 0;
    for (int r = 0; r < 256; r++) {
        for (int g = 0; g < 256; g++) {
            for (int b = 0; b < 256; b++) {
                color_t c = { (uint8_t)r, (uint8_t)g, (uint8_t)b, 255 };
                color_t hsl = color_hsl_to_rgb(color_rgb_to_hsl(c));
                color_t hsv = color_hsv_to_rgb(color_rgb_to_hsv(c));
                int dh = max3(channel_diff(hsl.r, r), channel_diff(hsl.g, g),
                              channel_diff(hsl.b, b));
                int dv = max3(channel_diff(hsv.r, r), channel_diff(hsv.g, g),
                              channel_diff(hsv.b, b));
                if (dh > worst_hsl) worst_hsl = dh;
                if (dv > worst_hsv) worst_hsv = dv;
            }
        }
    }
    assert(worst_hsl <= ROUNDTRIP_TOL);
    assert(worst_hsv <= ROUNDTRIP_TOL);
}

static void test_utils(void) {
    color_t c = COLOR_RGBA(200, 100, 50, 120);

    // factor 0 = tetap; alpha selalu dipertahankan.
    assert(color_eq(color_darken(c, 0), c));
    assert(color_eq(color_lighten(c, 0), c));
    assert(color_darken(c, 255).a == 120);
    assert(color_lighten(c, 255).a == 120);

    // Ujung skala.
    assert(color_eq(color_darken(c, 255), COLOR_RGBA(0, 0, 0, 120)));
    assert(color_eq(color_lighten(c, 255), COLOR_RGBA(255, 255, 255, 120)));

    // Arah & urutan kecerahan.
    color_t dark = color_darken(c, 128);
    color_t light = color_lighten(c, 128);
    assert(dark.r < c.r && dark.g < c.g && dark.b < c.b);
    assert(light.r > c.r && light.g > c.g && light.b > c.b);

    // Auto-contrast teks (Rec.601).
    assert(color_eq(color_get_contrast_text(COLOR_BLACK), COLOR_WHITE));
    assert(color_eq(color_get_contrast_text(COLOR_WHITE), COLOR_BLACK));
    assert(color_eq(color_get_contrast_text(COLOR_RED), COLOR_WHITE));
    assert(color_eq(color_get_contrast_text(COLOR_GREEN), COLOR_BLACK));
    assert(color_eq(color_get_contrast_text(COLOR_BLUE), COLOR_WHITE));
    assert(color_eq(color_get_contrast_text(COLOR_GRAY), COLOR_BLACK));
}

static void test_palette(void) {
    assert(rgb24(COLOR_TRANSPARENT) == 0x000000 && COLOR_TRANSPARENT.a == 0);
    assert(rgb24(COLOR_BLACK) == 0x000000 && COLOR_BLACK.a == 255);
    assert(rgb24(COLOR_WHITE) == 0xFFFFFF && COLOR_WHITE.a == 255);
    assert(rgb24(COLOR_RED) == 0xFF0000);
    assert(rgb24(COLOR_GREEN) == 0x00FF00);
    assert(rgb24(COLOR_BLUE) == 0x0000FF);
    assert(rgb24(COLOR_YELLOW) == 0xFFFF00);
    assert(rgb24(COLOR_CYAN) == 0x00FFFF);
    assert(rgb24(COLOR_MAGENTA) == 0xFF00FF);
    assert(rgb24(COLOR_GRAY) == 0x808080 && COLOR_GRAY.a == 255);

    // Bentuk INITIALIZER: yang penting konstanta (sah untuk `static const` di C)
    // dan nilainya sama dengan bentuk nilai runtime COLOR_RGB()/palet.
    static const color_t init_rgb   = COLOR_RGB_INIT(0x2D, 0x2D, 0x2D);   // tema notepad
    static const color_t init_prompt = COLOR_RGB_INIT(0x7C, 0xC7, 0xFF);  // prompt terminal
    static const color_t init_white = COLOR_WHITE_INIT;
    static const color_t init_black = COLOR_BLACK_INIT;
    static const color_t init_clear = COLOR_TRANSPARENT_INIT;
    static const color_t init_rgba  = COLOR_RGBA_INIT(10, 20, 30, 40);
    assert(color_eq(init_rgb, COLOR_RGB(0x2D, 0x2D, 0x2D)));
    assert(color_eq(init_prompt, COLOR_RGB(0x7C, 0xC7, 0xFF)));
    assert(color_eq(init_white, COLOR_WHITE) && color_eq(init_black, COLOR_BLACK));
    assert(color_eq(init_clear, COLOR_TRANSPARENT));
    assert(init_rgb.a == 255 && init_rgba.a == 40);
    assert(color_to_u32(init_rgb, FORMAT_ARGB) == 0xFF2D2D2Du);
    assert(color_to_u32(init_prompt, FORMAT_ARGB) == 0xFF7CC7FFu);
    // Komponen di luar uint8_t dipangkas, sama seperti COLOR_RGBA().
    static const color_t init_clip = COLOR_RGBA_INIT(0x1FF, 0x2FF, 0x3FF, 0x400);
    assert(color_eq(init_clip, COLOR_RGBA(0x1FF, 0x2FF, 0x3FF, 0x400)));
}

int main(void) {
    test_types();
    test_blend();
    test_aa_mix_parity();
    test_hsl();
    test_hsv();
    test_roundtrip_sweep();
    test_utils();
    test_palette();
    printf("color: OK\n");
    return 0;
}
