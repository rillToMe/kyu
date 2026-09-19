#ifndef COLOR_TYPES_H
#define COLOR_TYPES_H

#include <stdint.h>

// ============================================================
// libs/color — Tipe dasar warna (kernel + user space)
//
// color_t = RGBA 8-bit per kanal, non-premultiplied. Sesuai model display
// KyuzenOS (display.h): a=0 transparan, a!=0 opaque. Tidak ada alokasi.
// color_format_t hanya untuk serialisasi ke uint32_t framebuffer hardware;
// urutan byte mengikuti memori little-endian x86_64.
// ============================================================

typedef struct {
    uint8_t r, g, b, a;
} color_t;

typedef enum {
    FORMAT_ARGB = 0,   // byte memori B G R A → uint32 0xAARRGGBB (format display KyuzenOS)
    FORMAT_RGBA,       // byte memori A B G R → uint32 0xRRGGBBAA
    FORMAT_ABGR,       // byte memori R G B A → uint32 0xAABBGGRR
    FORMAT_BGRA        // byte memori A R G B → uint32 0xBBGGRRAA
} color_format_t;

// Konstruktor nilai — dipakai makro di bawah agar tetap valid di C++ (tanpa
// compound literal) dan tidak butuh storage.
static inline color_t color_make(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    color_t c = { r, g, b, a };
    return c;
}

#define COLOR_RGB(r, g, b)      color_make((uint8_t)(r), (uint8_t)(g), (uint8_t)(b), 255u)
#define COLOR_RGBA(r, g, b, a)  color_make((uint8_t)(r), (uint8_t)(g), (uint8_t)(b), (uint8_t)(a))

// color_t → pixel framebuffer sesuai format hardware.
static inline uint32_t color_to_u32(color_t c, color_format_t fmt) {
    switch (fmt) {
    case FORMAT_RGBA:
        return ((uint32_t)c.r << 24) | ((uint32_t)c.g << 16) | ((uint32_t)c.b << 8) | c.a;
    case FORMAT_ABGR:
        return ((uint32_t)c.a << 24) | ((uint32_t)c.b << 16) | ((uint32_t)c.g << 8) | c.r;
    case FORMAT_BGRA:
        return ((uint32_t)c.b << 24) | ((uint32_t)c.g << 16) | ((uint32_t)c.r << 8) | c.a;
    case FORMAT_ARGB:
    default:
        return ((uint32_t)c.a << 24) | ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
    }
}

// Kebalikan color_to_u32 — decode pixel (PNG, screenshot, capture, dll).
static inline color_t color_from_u32(uint32_t v, color_format_t fmt) {
    switch (fmt) {
    case FORMAT_RGBA:
        return color_make((uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v);
    case FORMAT_ABGR:
        return color_make((uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24));
    case FORMAT_BGRA:
        return color_make((uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24), (uint8_t)v);
    case FORMAT_ARGB:
    default:
        return color_make((uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v, (uint8_t)(v >> 24));
    }
}

#endif // COLOR_TYPES_H
