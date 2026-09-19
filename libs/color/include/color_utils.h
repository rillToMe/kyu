#ifndef COLOR_UTILS_H
#define COLOR_UTILS_H

#include "color_types.h"

// ============================================================
// libs/color — Utility warna UI (zero-alloc, kernel + user space)
//
// static const di header: tiap TU dapat salinan sendiri (tak ada storage
// global bersama), valid di C maupun C++, dan tidak butuh compound literal.
// ============================================================

static const color_t COLOR_TRANSPARENT = {   0,   0,   0,   0 };
static const color_t COLOR_BLACK       = {   0,   0,   0, 255 };
static const color_t COLOR_WHITE       = { 255, 255, 255, 255 };
static const color_t COLOR_RED         = { 255,   0,   0, 255 };
static const color_t COLOR_GREEN       = {   0, 255,   0, 255 };
static const color_t COLOR_BLUE        = {   0,   0, 255, 255 };
static const color_t COLOR_YELLOW      = { 255, 255,   0, 255 };
static const color_t COLOR_CYAN        = {   0, 255, 255, 255 };
static const color_t COLOR_MAGENTA     = { 255,   0, 255, 255 };
static const color_t COLOR_GRAY        = { 128, 128, 128, 255 };

#ifdef __cplusplus
extern "C" {
#endif

// factor 0..255: 0 = warna tetap, 255 = hitam (darken) / putih (lighten).
// Alpha warna asal dipertahankan.
color_t color_darken(color_t c, uint8_t factor);
color_t color_lighten(color_t c, uint8_t factor);

// Warna teks paling kontras (COLOR_BLACK / COLOR_WHITE) untuk latar bg,
// dari luminance Rec.601 integer: (299r + 587g + 114b) / 1000.
color_t color_get_contrast_text(color_t bg);

#ifdef __cplusplus
}
#endif

#endif // COLOR_UTILS_H
