#include "color_utils.h"

#include "color_blend.h"

// Ambang luminance ≥ setengah skala → teks hitam, di bawahnya teks putih.
#define CONTRAST_LUMA_THRESHOLD 128u

color_t color_darken(color_t c, uint8_t factor) {
    uint32_t keep = 255u - factor;
    color_t out;
    out.r = (uint8_t)color_div255((uint32_t)c.r * keep);
    out.g = (uint8_t)color_div255((uint32_t)c.g * keep);
    out.b = (uint8_t)color_div255((uint32_t)c.b * keep);
    out.a = c.a;
    return out;
}

color_t color_lighten(color_t c, uint8_t factor) {
    color_t out;
    out.r = (uint8_t)(c.r + color_div255((uint32_t)(255u - c.r) * factor));
    out.g = (uint8_t)(c.g + color_div255((uint32_t)(255u - c.g) * factor));
    out.b = (uint8_t)(c.b + color_div255((uint32_t)(255u - c.b) * factor));
    out.a = c.a;
    return out;
}

color_t color_get_contrast_text(color_t bg) {
    uint32_t luma = (299u * bg.r + 587u * bg.g + 114u * bg.b) / 1000u;
    return (luma >= CONTRAST_LUMA_THRESHOLD) ? COLOR_BLACK : COLOR_WHITE;
}
