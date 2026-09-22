#ifndef KYUZEN_MEDIA_SCALE_H
#define KYUZEN_MEDIA_SCALE_H

// ============================================================
// media_scale.h — skalasi RGBA integer untuk thumbnail/ikon.
//
// SATU implementasi untuk desktop (apps/desktop/app_icons.cpp) dan aplikasi
// media (Gallery). Dulu hanya ada sebagai `scale_nearest()` lokal di
// app_icons.cpp; Gallery butuh algoritma yang sama persis untuk thumbnail —
// menyalinnya berarti dua jalur downscale yang bisa menyimpang, jadi isinya
// dipindah ke header `static inline` (pola yang sama dengan `build_app_path`
// di userlib.h dan aa_math.h: util kecil tanpa dependensi = header, bukan .o).
//
// Murni integer, tanpa float/SSE (build -msoft-float -mno-sse) → aman
// dikompilasi host (test) maupun target bare-metal.
// ============================================================

#include <stdint.h>

// src (sw×sh) → dst (dw×dh), piksel ARGB8888 (alpha di byte atas).
// Keluarannya straight alpha (RGB sudah dibagi alpha) — siap di-blend.
//
// Upscale: nearest murni — tajam, murah.
// Downscale: rata-rata box per piksel tujuan, RGB dirata-rata dengan bobot
// alpha (premultiplied) supaya tepi transparan tidak berdarah hitam; box
// transparan penuh → 0. Nearest biasa akan me-skip baris/kolom sumber
// (256→48 = tiap ~5px) sehingga garis tipis hilang — inilah alasan versi box.
//
// Box menyelamatkan garis tipis tapi kontrasnya turun (~15% di bawah acuan
// Lanczos untuk ikon 256→48) sehingga hasilnya tampak lembek; penajaman
// opsionalnya ada di media_sharpen_rgba di bawah.
static inline void media_scale_rgba(const uint32_t* src, int sw, int sh,
                                    uint32_t* dst, int dw, int dh) {
    if (!src || !dst || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    if (dw >= sw && dh >= sh) {
        for (int y = 0; y < dh; y++) {
            int sy = y * sh / dh;
            for (int x = 0; x < dw; x++)
                dst[y * dw + x] = src[sy * sw + (x * sw / dw)];
        }
        return;
    }
    for (int y = 0; y < dh; y++) {
        int y0 = y * sh / dh;
        int y1 = (y + 1) * sh / dh;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int x0 = x * sw / dw;
            int x1 = (x + 1) * sw / dw;
            if (x1 <= x0) x1 = x0 + 1;
            uint32_t sum_a = 0, sum_r = 0, sum_g = 0, sum_b = 0;
            for (int sy = y0; sy < y1; sy++) {
                for (int sx = x0; sx < x1; sx++) {
                    uint32_t p = src[sy * sw + sx];
                    uint32_t a = p >> 24;
                    sum_a += a;
                    sum_r += ((p >> 16) & 0xFFu) * a;
                    sum_g += ((p >> 8) & 0xFFu) * a;
                    sum_b += (p & 0xFFu) * a;
                }
            }
            if (sum_a == 0) {
                dst[y * dw + x] = 0;
                continue;
            }
            uint32_t n = (uint32_t)(x1 - x0) * (uint32_t)(y1 - y0);
            uint32_t a = sum_a / n;
            uint32_t r = sum_r / sum_a;
            uint32_t g = sum_g / sum_a;
            uint32_t b = sum_b / sum_a;
            dst[y * dw + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

// Unsharp mask 3x3 IN-PLACE untuk hasil downscale yang tampak lembek
// (ikon 48px: detail sudah dibawa box, ambang tepinya yang perlu dinaikkan).
//
//   p' = p + k*(p - blur3x3(p))     k = amount_pct/100
//
// Dikerjakan pada komponen PREMULTIPLIED (r*a) supaya tetangga transparan
// tidak menarik warna ke arah hitam (halo), lalu dikembalikan ke straight
// alpha. Karena blur selalu berada di dalam rentang lokal, hasilnya tidak
// pernah overshoot: tajam tanpa ringing/halo, jadi amount besar pun aman.
// Alpha ikut dipertajam (siluet ikon jadi lebih tegas) dan tetap konsisten
// dengan warna: r' selalu <= 255.
//
//   amount_pct  0 = tanpa perubahan;
//   rowbuf     scratch 3 baris milik pemanggil (rowbuf_n >= 3*w).
//
// Baris asli y-1/y/y+1 disalin ke cincin 3 baris ini, jadi output boleh
// ditulis in-place tanpa buffer kedua seukuran gambar (wallpaper 1080p tidak
// lewat sini justru karena itu). Tanpa float/SSE, aman host & bare-metal.
static inline void media_sharpen_rgba(uint32_t* px, int w, int h,
                                      uint32_t* rowbuf, int rowbuf_n,
                                      int amount_pct) {
    if (!px || !rowbuf || w <= 0 || h <= 0 || amount_pct <= 0) return;
    if (rowbuf_n < 3 * w) return;  // scratch kurang: lewati, bukan alokasi
    const int lastx = w - 1;
    uint32_t* ring[3] = {rowbuf, rowbuf + w, rowbuf + 2 * w};
    for (int x = 0; x < w; x++) {
        const uint32_t v = px[x];
        ring[0][x] = v;  // baris -1 = baris 0 (replikasi tepi)
        ring[1][x] = v;  // baris 0
    }
    const int row1 = (h > 1) ? 1 : 0;
    for (int x = 0; x < w; x++) ring[2][x] = px[row1 * w + x];  // baris +1
    int i0 = 0, i1 = 1, i2 = 2;  // cincin: baris y-1, y, y+1
    for (int y = 0; y < h; y++) {
        const uint32_t* up = ring[i0];
        const uint32_t* mid = ring[i1];
        const uint32_t* dn = ring[i2];
        for (int x = 0; x < w; x++) {
            const int xm = (x > 0) ? x - 1 : 0;
            const int xp = (x < lastx) ? x + 1 : lastx;
            uint32_t acc_a = 0, acc_r = 0, acc_g = 0, acc_b = 0;
            for (int k = 0; k < 3; k++) {
                const uint32_t* row = (k == 0) ? up : ((k == 1) ? mid : dn);
                for (int j = 0; j < 3; j++) {
                    const uint32_t v = row[(j == 0) ? xm : ((j == 1) ? x : xp)];
                    const uint32_t a = v >> 24;
                    acc_a += a;
                    acc_r += ((v >> 16) & 0xFFu) * a;
                    acc_g += ((v >> 8) & 0xFFu) * a;
                    acc_b += (v & 0xFFu) * a;
                }
            }
            const uint32_t v = mid[x];
            const uint32_t a = v >> 24;
            // Cast gaya C: header ini ikut dikompilasi sebagai C (libs/media/media.c).
            int sa = (int)a + ((int)a - (int)(acc_a / 9)) * amount_pct / 100;
            if (sa < 0) sa = 0;
            if (sa > 255) sa = 255;
            if (sa == 0) {  // tetap transparan penuh
                px[y * w + x] = 0;
                continue;
            }
            const uint32_t pr = ((v >> 16) & 0xFFu) * a;
            const uint32_t pg = ((v >> 8) & 0xFFu) * a;
            const uint32_t pb = (v & 0xFFu) * a;
            int sr = (int)pr + ((int)pr - (int)(acc_r / 9)) * amount_pct / 100;
            int sg = (int)pg + ((int)pg - (int)(acc_g / 9)) * amount_pct / 100;
            int sb = (int)pb + ((int)pb - (int)(acc_b / 9)) * amount_pct / 100;
            const int hi = sa * 255;
            if (sr < 0) sr = 0; else if (sr > hi) sr = hi;
            if (sg < 0) sg = 0; else if (sg > hi) sg = hi;
            if (sb < 0) sb = 0; else if (sb > hi) sb = hi;
            px[y * w + x] = ((uint32_t)sa << 24) | ((uint32_t)(sr / sa) << 16) |
                            ((uint32_t)(sg / sa) << 8) | (uint32_t)(sb / sa);
        }
        // Ganti cincin: slot terlama diisi baris y+2 (klamp baris terakhir).
        const int t = i0;
        i0 = i1;
        i1 = i2;
        i2 = t;
        if (y + 1 < h) {
            int ny = y + 2;
            if (ny > h - 1) ny = h - 1;
            for (int x = 0; x < w; x++) ring[i2][x] = px[ny * w + x];
        }
    }
}

// Hitung ukuran tujuan yang mempertahankan rasio aspek di dalam kotak
// (box_w × box_h), tanpa upscale (hasil tidak pernah lebih besar dari asli).
// Menulis *out_w / *out_h (minimal 1). Return 1 bila ada gambar, 0 bila tidak.
static inline int media_fit_box(int sw, int sh, int box_w, int box_h,
                                int* out_w, int* out_h) {
    if (sw <= 0 || sh <= 0 || box_w <= 0 || box_h <= 0) return 0;
    int dw = sw, dh = sh;
    if (dw > box_w) { dh = dh * box_w / dw; dw = box_w; }
    if (dh > box_h) { dw = dw * box_h / dh; dh = box_h; }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    if (out_w) *out_w = dw;
    if (out_h) *out_h = dh;
    return 1;
}

#endif // KYUZEN_MEDIA_SCALE_H
