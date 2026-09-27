// Host test: libs/gui/color — konstruktor HEX + ekuivalensi RGB.
//
// Dibangun: make test-color (lihat Makefile: COLOR_SRCS + color_cxx_check).
// File ini pernah hilang dari tree (dirujuk Makefile/docs); recreate minimal
// untuk mengunci API COLOR_HEX(0xRRGGBB) — ekstraksi channel, urutan R/G/B,
// alpha opaque, ekuivalensi dengan COLOR_RGB_INIT/COLOR_RGB, dan perilaku
// constant-expression (static init + _Static_assert).
//
// Bukan sweep 256^3 / paritas aa_mix penuh (riwayat); fokus ke kontrak HEX:
//   COLOR_HEX(0xRRGGBB) == COLOR_RGB_INIT(R, G, B), A = 255.

#include <stdio.h>
#include <string.h>

#include "color_types.h"
#include "color_utils.h"

static int g_fail = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("PASS %s\n", name); } \
    else { printf("FAIL %s\n", name); g_fail++; } \
} while (0)

// --- Static-init form: harus constant expression di C ---
static const color_t HEX_ZERO = COLOR_HEX(0x000000);
static const color_t HEX_FULL = COLOR_HEX(0xFFFFFF);
static const color_t HEX_1E = COLOR_HEX(0x1E1E1E);
static const color_t HEX_ACC = COLOR_HEX(0x0098BC);
static const color_t HEX_HOV = COLOR_HEX(0x3E3E42);

static const color_t INIT_1E = COLOR_RGB_INIT(0x1E, 0x1E, 0x1E);

// Compile-time proof: ekstraksi channel + ekuivalensi INIT.
_Static_assert(HEX_ZERO.r == 0 && HEX_ZERO.g == 0 && HEX_ZERO.b == 0 &&
               HEX_ZERO.a == 255, "HEX 0x000000");
_Static_assert(HEX_FULL.r == 255 && HEX_FULL.g == 255 && HEX_FULL.b == 255 &&
               HEX_FULL.a == 255, "HEX 0xFFFFFF");
_Static_assert(HEX_1E.r == 0x1E && HEX_1E.g == 0x1E && HEX_1E.b == 0x1E &&
               HEX_1E.a == 255, "HEX 0x1E1E1E");
_Static_assert(HEX_ACC.r == 0x00 && HEX_ACC.g == 0x98 && HEX_ACC.b == 0xBC &&
               HEX_ACC.a == 255, "HEX 0x0098BC");
_Static_assert(HEX_1E.r == INIT_1E.r && HEX_1E.g == INIT_1E.g &&
               HEX_1E.b == INIT_1E.b && HEX_1E.a == INIT_1E.a,
               "HEX == INIT");

static int eq(color_t a, color_t b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

int main(void) {
    // RGB extraction (runtime assert melengkapi _Static_assert di atas).
    CHECK(eq(HEX_ZERO, COLOR_RGB(0, 0, 0)), "hex 0x000000 -> 0,0,0,255");
    CHECK(eq(HEX_FULL, COLOR_RGB(255, 255, 255)), "hex 0xFFFFFF -> 255,255,255,255");
    CHECK(eq(HEX_1E, COLOR_RGB(0x1E, 0x1E, 0x1E)), "hex 0x1E1E1E -> 30,30,30,255");
    CHECK(eq(HEX_ACC, COLOR_RGB(0x00, 0x98, 0xBC)), "hex 0x0098BC -> 0,152,188,255");
    CHECK(eq(HEX_HOV, COLOR_RGB(0x3E, 0x3E, 0x42)), "hex 0x3E3E42");

    // Equivalence: static INIT + runtime RGB.
    CHECK(eq(HEX_1E, INIT_1E), "hex == INIT (static)");
    CHECK(eq(COLOR_RGB(0x1E, 0x1E, 0x1E), INIT_1E), "rgb == INIT (sanity)");

    // Local-init form (`color_t c = COLOR_HEX(..)`).
    {
        color_t c = COLOR_HEX(0x2D2D2D);
        CHECK(eq(c, COLOR_RGB(0x2D, 0x2D, 0x2D)), "hex local-init");
    }

    // Channel ordering edges: tiap byte harus mendarat di kanal yang benar.
    // (COLOR_HEX braced-list: ikat ke local dulu, bukan argumen fungsi di C.)
    { color_t e = COLOR_HEX(0x000001);
      CHECK(eq(e, COLOR_RGB(0, 0, 1)), "edge 0x000001 -> B=1"); }
    { color_t e = COLOR_HEX(0x010000);
      CHECK(eq(e, COLOR_RGB(1, 0, 0)), "edge 0x010000 -> R=1"); }
    { color_t e = COLOR_HEX(0x00FF00);
      CHECK(eq(e, COLOR_RGB(0, 255, 0)), "edge 0x00FF00 -> G=255"); }
    { color_t e = COLOR_HEX(0xFF0000);
      CHECK(eq(e, COLOR_RGB(255, 0, 0)), "edge 0xFF0000 -> R=255"); }
    { color_t e = COLOR_HEX(0x123456);
      CHECK(eq(e, COLOR_RGB(0x12, 0x34, 0x56)), "edge 0x123456"); }

    // Alpha selalu opaque; serialisasi ARGB = 0xFFRRGGBB (kontrak display).
    CHECK(HEX_1E.a == 255, "hex alpha opaque");
    CHECK(color_to_u32(HEX_1E, FORMAT_ARGB) == 0xFF1E1E1Eu,
          "hex serializes 0xFF1E1E1E");
    CHECK(eq(color_from_u32(0xFF0098BCu, FORMAT_ARGB), HEX_ACC),
          "hex round-trip u32");

    // Palet existing tak tersentuh.
    CHECK(eq(COLOR_WHITE, COLOR_RGB(255, 255, 255)), "palette white intact");
    CHECK(eq(COLOR_BLACK, COLOR_RGB(0, 0, 0)), "palette black intact");

    // color_hex(): bentuk ekspresi runtime — nilai identik dengan COLOR_HEX,
    // bisa dipakai sebagai argumen fungsi / sisi kanan assignment.
    CHECK(eq(color_hex(0x1E1E1E), HEX_1E), "color_hex == COLOR_HEX");
    CHECK(eq(color_hex(0xFFFFFF), HEX_FULL), "color_hex white");
    CHECK(eq(color_hex(0x123456), COLOR_RGB(0x12, 0x34, 0x56)),
          "color_hex channel order");
    { color_t h = color_hex(0x0098BC);
      CHECK(h.a == 255, "color_hex alpha opaque"); }
    { uint32_t v = 0x2A4A7E;   // evaluasi tunggal: argumen ekspresi aman
      CHECK(eq(color_hex(v++), COLOR_RGB(0x2A, 0x4A, 0x7E)),
            "color_hex value");
      CHECK(v == 0x2A4A7Fu, "color_hex single-eval"); }

    if (g_fail == 0) printf("color_test: ALL PASS\n");
    else printf("color_test: %d FAILURES\n", g_fail);
    return g_fail ? 1 : 0;
}
