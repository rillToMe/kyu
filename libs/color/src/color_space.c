#include "color_space.h"

static uint8_t channel_max(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t m = (r > g) ? r : g;
    return (m > b) ? m : b;
}

static uint8_t channel_min(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t m = (r < g) ? r : g;
    return (m < b) ? m : b;
}

// Pembagian bulat ke arah nol, dibulatkan ke terdekat (num/den bisa negatif).
static int32_t div_round(int32_t num, int32_t den) {
    if (num >= 0) return (num + den / 2) / den;
    return -((-num + den / 2) / den);
}

// Hue 0..359 dari kanal max & delta (delta != 0). Bobot 60 derajat per sektor.
// Dibulatkan: truncate 1 derajat saja sudah menggeser kanal jauh sampai ~4.
static uint16_t hue_from_rgb(uint8_t r, uint8_t g, uint8_t b, uint8_t max, uint8_t delta) {
    int32_t h;
    if (max == r)      h = div_round(60 * ((int32_t)g - (int32_t)b), delta);
    else if (max == g) h = 120 + div_round(60 * ((int32_t)b - (int32_t)r), delta);
    else               h = 240 + div_round(60 * ((int32_t)r - (int32_t)g), delta);
    if (h < 0) h += 360;
    return (uint16_t)h;
}

// Kanal RGB untuk posisi hue t (0..479, di-normalisasi ke 0..359).
// p = batas bawah, q = batas atas interpolasi linier antar sektor hue.
static uint8_t hue_channel(uint16_t t, uint8_t p, uint8_t q) {
    if (t >= 360) t = (uint16_t)(t - 360);
    if (t < 60)  return (uint8_t)(p + div_round((int32_t)(q - p) * t, 60));
    if (t < 180) return q;
    if (t < 240) return (uint8_t)(p + div_round((int32_t)(q - p) * (240 - t), 60));
    return p;
}

color_hsl_t color_rgb_to_hsl(color_t c) {
    uint8_t max = channel_max(c.r, c.g, c.b);
    uint8_t min = channel_min(c.r, c.g, c.b);
    uint8_t delta = (uint8_t)(max - min);
    uint8_t l = (uint8_t)((max + min + 1) / 2);
    color_hsl_t hsl = { 0, 0, l };
    if (delta == 0) return hsl;
    hsl.h = hue_from_rgb(c.r, c.g, c.b, max, delta);
    // s = delta / (2l) bila l < 0.5, delta / (2 - 2l) bila l >= 0.5.
    uint32_t denom = (l < 128) ? (uint32_t)(max + min) : (510u - max - min);
    hsl.s = (uint8_t)(((uint32_t)delta * 255u + denom / 2) / denom);
    return hsl;
}

color_t color_hsl_to_rgb(color_hsl_t hsl) {
    if (hsl.s == 0) return color_make(hsl.l, hsl.l, hsl.l, 255);
    uint32_t ls = ((uint32_t)hsl.l * hsl.s + 127u) / 255u;
    uint8_t q = (hsl.l < 128) ? (uint8_t)(hsl.l + ls)
                              : (uint8_t)((uint32_t)hsl.l + hsl.s - ls);
    uint8_t p = (uint8_t)(2u * hsl.l - q);
    uint16_t h = (uint16_t)(hsl.h % 360);
    return color_make(hue_channel((uint16_t)((h + 120u) % 360u), p, q),
                      hue_channel(h, p, q),
                      hue_channel((uint16_t)((h + 240u) % 360u), p, q),
                      255);
}

color_hsv_t color_rgb_to_hsv(color_t c) {
    uint8_t max = channel_max(c.r, c.g, c.b);
    uint8_t min = channel_min(c.r, c.g, c.b);
    uint8_t delta = (uint8_t)(max - min);
    color_hsv_t hsv = { 0, 0, max };
    if (max == 0) return hsv;
    hsv.s = (uint8_t)(((uint32_t)delta * 255u + max / 2) / max);
    if (delta == 0) return hsv;
    hsv.h = hue_from_rgb(c.r, c.g, c.b, max, delta);
    return hsv;
}

color_t color_hsv_to_rgb(color_hsv_t hsv) {
    if (hsv.s == 0) return color_make(hsv.v, hsv.v, hsv.v, 255);
    uint16_t h = (uint16_t)(hsv.h % 360);
    uint32_t sector = h / 60;   // 0..5
    uint32_t f = h % 60;        // 0..59
    uint8_t v = hsv.v;
    uint8_t p = (uint8_t)(((uint32_t)v * (255u - hsv.s) + 127u) / 255u);
    uint8_t q = (uint8_t)(((uint32_t)v * (255u - (uint32_t)hsv.s * f / 60u) + 127u) / 255u);
    uint8_t t = (uint8_t)(((uint32_t)v * (255u - (uint32_t)hsv.s * (60u - f) / 60u) + 127u) / 255u);
    switch (sector) {
    case 0:  return color_make(v, t, p, 255);
    case 1:  return color_make(q, v, p, 255);
    case 2:  return color_make(p, v, t, 255);
    case 3:  return color_make(p, q, v, 255);
    case 4:  return color_make(t, p, v, 255);
    default: return color_make(v, p, q, 255);
    }
}
