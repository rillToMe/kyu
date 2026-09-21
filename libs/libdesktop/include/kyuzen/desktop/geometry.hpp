// libdesktop — geometri + warna (header-only, tanpa dependensi apa pun).
//
// Tipe trivial untuk dipakai widget/window/layout/input/rendering kelak.
// Bukan library geometri: hanya yang dibutuhkan desktop hari ini.
#ifndef KYUZEN_DESKTOP_GEOMETRY_HPP
#define KYUZEN_DESKTOP_GEOMETRY_HPP

#include <stdint.h>

namespace kyuzen {
namespace desktop {

struct Point {
    int x;
    int y;
};

struct Size {
    int width;
    int height;
};

struct Rect {
    int x;
    int y;
    int width;
    int height;

    bool contains(Point p) const {
        return p.x >= x && p.x < x + width && p.y >= y && p.y < y + height;
    }
    bool intersects(const Rect& o) const {
        return x < o.x + o.width && o.x < x + width &&
               y < o.y + o.height && o.y < y + height;
    }
};

// Warna milik framework (RGBA per komponen). Backend Canvas mengonversinya
// ke tipe warna renderer; aplikasi TIDAK pernah menyentuh header warna C.
struct Color {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
};

// constexpr agar objek namespace-scope (palet theme) terinisialisasi
// statis — tanpa ctor dinamis, tanpa ketergantungan .init_array.
inline constexpr Color rgb(uint8_t r, uint8_t g, uint8_t b) {
    return Color{r, g, b, 255};
}

}  // namespace desktop
}  // namespace kyuzen

#endif
