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

// Salinan dengan alpha/cakupan diganti (komponen lain utuh).
static inline color_t color_with_alpha(color_t c, uint8_t a) {
    c.a = a;
    return c;
}

// Salinan dipaksa opaque — permukaan kanvas XRGB diperlakukan buram sebelum
// di-blend (byte tinggi kanvas = mask transparansi, bukan alpha).
static inline color_t color_opaque(color_t c) {
    c.a = 255;
    return c;
}

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

// Bentuk INITIALIZER: daftar komponen, untuk `static const` di C.
// color_make()/COLOR_RGB() menghasilkan nilai lewat panggilan inline — bukan
// constant expression di C — jadi inisialisasi statik memakai makro ini:
//   static const color_t C = COLOR_RGB_INIT(0x2D, 0x2D, 0x2D);
//   static const ui_theme_t T = { COLOR_RGB_INIT(...), COLOR_WHITE_INIT, ... };
// Hanya sah di posisi initializer (bukan sebagai nilai dalam ekspresi biasa).
#define COLOR_RGB_INIT(r, g, b)      { (uint8_t)(r), (uint8_t)(g), (uint8_t)(b), 255u }
#define COLOR_RGBA_INIT(r, g, b, a)  { (uint8_t)(r), (uint8_t)(g), (uint8_t)(b), (uint8_t)(a) }

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
