// Host check: libs/gui/color headers sebagai C++17 (fsyntax-only).
//
// Dibangun: make test-color (langkah HOSTCXX). Mengunci COLOR_HEX di posisi
// C++: namespace-scope static, theme-struct init, mem-init, assignment,
// function-arg — semua tanpa compound literal / dynamic-init.

#include "color_types.h"
#include "color_utils.h"

struct fake_theme {
    color_t bg;
    color_t fg;
};

constexpr color_t K = COLOR_HEX(0x1E1E1E);
constexpr fake_theme T = { COLOR_HEX(0x2D2D2D), COLOR_HEX(0xD4D4D4) };

static_assert(K.r == 0x1E && K.g == 0x1E && K.b == 0x1E && K.a == 255,
              "HEX static");
static_assert(T.bg.r == 0x2D && T.fg.r == 0xD4, "HEX theme table");

struct holder {
    color_t c;
    holder()
        : c(COLOR_HEX(0x0098BC)) {}
};

static color_t take(color_t c) { return c; }

int main() {
    holder h;
    bool ok = h.c.g == 0x98 && h.c.b == 0xBC;
    color_t a = COLOR_HEX(0xFFFFFF);
    a = COLOR_HEX(0x3E3E42);
    color_t b = take(COLOR_HEX(0x7CC7FF));
    ok = ok && a.r == 0x3E && b.r == 0x7C;
    color_t w = COLOR_WHITE;
    ok = ok && w.a == 255;
    return ok ? 0 : 1;
}
