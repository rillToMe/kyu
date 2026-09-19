// libs/widget/include/runtime/platform.hpp — header platform bare-metal.
// Baru saat split: menyatukan wrapper extern "C" yang dulu ada di kepala
// apps/libui.cpp supaya syscall/libgui/libs-color tetap berlinkage C ketika
// dikompilasi sebagai C++. Urutan include & komentar identik dengan aslinya.
#ifndef KWIDGET_RUNTIME_PLATFORM_HPP
#define KWIDGET_RUNTIME_PLATFORM_HPP

#include <stdint.h>
#include <stddef.h>

extern "C" {
#include "userlib.h"
#include "libgui.h"
// libs/color — C/C++ compatible; di dalam extern "C" agar fungsi out-of-line
// (color_blend_span) tetap berlinkage C.
#include "color_types.h"
#include "color_blend.h"
#include "color_utils.h"
}

// libui.h punya guard extern "C" sendiri — aman di-include dari C++.
#include "libui.h"

// Coverage AA (aa_cov) tetap dari include/aa_math.h; blending warna libui kini
// lewat libs/color. Semua static inline, integer saja — aman di-include C++.
#include "aa_math.h"

#endif // KWIDGET_RUNTIME_PLATFORM_HPP
