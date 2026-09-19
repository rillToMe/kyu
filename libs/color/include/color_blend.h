#ifndef COLOR_BLEND_H
#define COLOR_BLEND_H

#include "color_types.h"

// ============================================================
// libs/color — Blending (integer, tanpa floating point)
//
// Kernel & app dibangun dengan -mno-sse -mno-sse2 -msoft-float, jadi semua
// operasi memakai integer. Tidak ada alokasi.
// ============================================================

// Pembagian 255 tanpa instruksi divide; presisi untuk x < 65536.
static inline uint32_t color_div255(uint32_t x) {
    return (x + 1u + (x >> 8)) >> 8;
}

// Overlay src di atas dst (non-premultiplied, a = cakupan 0..255).
// src.a == 0 → dst apa adanya (tidak menggambar); dst.a == 0 → src (kanvas
// kosong); selain itu campuran RGB dengan hasil opaque (a=255) — sesuai model
// mask display KyuzenOS yang hanya mengenal transparan/opaque.
static inline color_t color_blend_alpha(color_t src, color_t dst) {
    if (src.a == 0) return dst;
    if (dst.a == 0) {
        color_t out = src;
        out.a = 255;
        return out;
    }
    if (src.a == 255) return src;
    uint32_t na = 255u - src.a;
    color_t out;
    out.r = (uint8_t)(color_div255((uint32_t)src.r * src.a) + color_div255((uint32_t)dst.r * na));
    out.g = (uint8_t)(color_div255((uint32_t)src.g * src.a) + color_div255((uint32_t)dst.g * na));
    out.b = (uint8_t)(color_div255((uint32_t)src.b * src.a) + color_div255((uint32_t)dst.b * na));
    out.a = 255;
    return out;
}

// Batch: dst[i] = blend(src[i], dst[i]).
// Satu-satunya seam SIMD (SSE2/AVX) nanti — cukup aktifkan COLOR_BLEND_USE_SIMD
// dan sediakan color_blend_span_simd() di file terpisah; pemanggil tak berubah.
void color_blend_span(color_t* dst, const color_t* src, uint32_t count);

#endif // COLOR_BLEND_H
