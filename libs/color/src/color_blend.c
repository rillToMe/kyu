#include "color_blend.h"

#ifdef COLOR_BLEND_USE_SIMD
// Disediakan file SIMD (mis. color_blend_sse2.c, dikompilasi dengan -msse2).
void color_blend_span_simd(color_t* dst, const color_t* src, uint32_t count);
#endif

void color_blend_span(color_t* dst, const color_t* src, uint32_t count) {
#ifdef COLOR_BLEND_USE_SIMD
    color_blend_span_simd(dst, src, count);
#else
    for (uint32_t i = 0; i < count; i++) {
        dst[i] = color_blend_alpha(src[i], dst[i]);
    }
#endif
}
