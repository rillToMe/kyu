// Compile-only check: header libs/color harus valid di-include dari C++
// (libs/widget/ adalah C++17; app userspace boleh memakai lib ini).
// Dijalankan oleh `make test-color` via clang++ -fsyntax-only.
//
// Catatan: makro COLOR_RGB/COLOR_RGBA sengaja memanggil fungsi inline, bukan
// compound literal C — itulah yang menjaga header ini tetap C/C++ compatible.

#include "color_blend.h"
#include "color_space.h"
#include "color_types.h"
#include "color_utils.h"

void color_cxx_check(void) {
    color_t c = COLOR_RGB(0x11, 0x22, 0x33);
    color_t blended = color_blend_alpha(COLOR_RGBA(255, 255, 255, 128), c);
    color_hsl_t hsl = color_rgb_to_hsl(blended);
    color_hsv_t hsv = color_rgb_to_hsv(blended);
    color_t shadow = color_darken(color_hsl_to_rgb(hsl), 64);
    color_t highlight = color_lighten(color_hsv_to_rgb(hsv), 64);
    color_t text = color_get_contrast_text(shadow);
    (void)highlight;
    (void)text;

    color_t array[2] = { c, blended };
    color_blend_span(array, array, 2);
    (void)color_to_u32(array[0], FORMAT_ARGB);
    (void)color_from_u32(0xFF7CC7FFu, FORMAT_BGRA);
}
