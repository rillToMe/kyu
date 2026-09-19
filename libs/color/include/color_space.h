#ifndef COLOR_SPACE_H
#define COLOR_SPACE_H

#include "color_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
// libs/color — Konversi ruang warna (integer murni)
//
// Skala: hue 0..359 derajat, saturation/lightness/value 0..255.
// Round-trip RGB -> HSL/HSV -> RGB bererror maksimum +-3 akibat kuantisasi
// 8-bit (dibuktikan sweep 256^3 warna di test/color_test.c).
// Alpha BUKAN bagian HSL/HSV; hsl_to_rgb/hsv_to_rgb mengembalikan a=255
// (caller bisa mengembalikan alpha aslinya sendiri).
// Berguna untuk UI picker, efek dinamis, dan turunan tema.
// ============================================================

typedef struct {
    uint16_t h;   // 0..359
    uint8_t  s;   // 0..255
    uint8_t  l;   // 0..255
} color_hsl_t;

typedef struct {
    uint16_t h;   // 0..359
    uint8_t  s;   // 0..255
    uint8_t  v;   // 0..255
} color_hsv_t;

color_hsl_t color_rgb_to_hsl(color_t c);
color_t     color_hsl_to_rgb(color_hsl_t hsl);
color_hsv_t color_rgb_to_hsv(color_t c);
color_t     color_hsv_to_rgb(color_hsv_t hsv);

#ifdef __cplusplus
}
#endif

#endif // COLOR_SPACE_H
