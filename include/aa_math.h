#ifndef AA_MATH_H
#define AA_MATH_H

// Math anti-alias (coverage) + blend warna untuk toolkit userspace
// (libs/widget/include/core/painter.hpp). Blending compositor kernel kini lewat libs/color
// (color_blend_alpha) dan di sini hanya memakai aa_cov.
//
// INTEGER SAJA: kernel dan app dibangun dengan -mno-sse -mno-sse2 -msoft-float
// (Makefile:76, user_apps/Makefile:41) dan kernel tidak pernah mengaktifkan
// OSFXSR/fxsave — tidak ada float/SSE yang boleh dipakai. Alpha memakai skala
// 0..255 (setara Q0.8), pembagian /255 saja.
//
// Self-check host (pure integer, tak butuh QEMU):
//   clang test/aa_math_test.c -o /tmp/aa && /tmp/aa

#include <stdint.h>

// dst = (src*a + dst*(255-a)) / 255 per kanal. Byte alpha input diabaikan;
// hasil selalu 0x00RRGGBB.
static inline uint32_t aa_mix(uint32_t src, uint32_t dst, uint32_t a) {
    if (a >= 255) return src & 0xFFFFFF;
    if (a == 0)   return dst & 0xFFFFFF;
    uint32_t na = 255 - a;
    uint32_t r = ((((src >> 16) & 0xFF) * a) + (((dst >> 16) & 0xFF) * na)) / 255;
    uint32_t g = ((((src >>  8) & 0xFF) * a) + (((dst >>  8) & 0xFF) * na)) / 255;
    uint32_t b = (((src & 0xFF) * a) + ((dst & 0xFF) * na)) / 255;
    return (r << 16) | (g << 8) | b;
}

// pct > 0 → mendekati putih, pct < 0 → mendekati hitam (persen jarak).
static inline uint32_t aa_shade(uint32_t c, int pct) {
    int r = (int)((c >> 16) & 0xFF), g = (int)((c >> 8) & 0xFF), b = (int)(c & 0xFF);
    if (pct >= 0) {
        r += (255 - r) * pct / 100; g += (255 - g) * pct / 100; b += (255 - b) * pct / 100;
    } else {
        r += r * pct / 100; g += g * pct / 100; b += b * pct / 100;
    }
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

// Coverage 0..255 pixel (px,py) di dalam rounded-rect (x,y,w,h) dengan radius
// sudut atas `rt` dan bawah `rb` (0 = kotak). Supersample 4x4 integer —
// pengganti Wu's algorithm yang butuh float.
// ponytail: 16 sample → 17 tingkat alpha; naikkan grid bila tepi terlihat
// bertingkat pada radius besar.
static inline uint32_t aa_cov(int px, int py, int x, int y, int w, int h,
                              int rt, int rb) {
    int r, cx, cy;
    if (rt > 0 && py < y + rt)              { r = rt; cy = y + rt; }
    else if (rb > 0 && py >= y + h - rb)    { r = rb; cy = y + h - rb; }
    else return 255;
    if (px < x + r) cx = x + r;
    else if (px >= x + w - r) cx = x + w - r;
    else return 255;
    int rr = (r * 8) * (r * 8), hit = 0;
    for (int sy = 1; sy < 8; sy += 2)
        for (int sx = 1; sx < 8; sx += 2) {
            int dx = px * 8 + sx - cx * 8, dy = py * 8 + sy - cy * 8;
            if (dx * dx + dy * dy <= rr) hit++;
        }
    return (uint32_t)(hit * 255 / 16);
}

#endif
