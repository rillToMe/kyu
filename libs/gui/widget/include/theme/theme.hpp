// libs/widget/include/theme/theme.hpp — pintu masuk tunggal design system.
//
// Satu include untuk seluruh bahasa visual KyuzenOS:
//
//   theme/colors.hpp      token warna + kebijakan turunan state
//   theme/typography.hpp  peran tipografi + pengukuran teks
//   theme/metrics.hpp     jarak, radius, ukuran kontrol, bar chrome
//   theme/elevation.hpp   model kedalaman + kebijakan bayangan
//   theme/motion.hpp      durasi + easing
//
// Palet yang SUDAH ter-resolve ada di `core/theme.hpp` (`struct Theme`), yang
// menyusun token di atas menjadi satu struct yang dibaca widget. Pemisahan ini
// disengaja: `theme/` = keputusan desain (statis), `core/theme.hpp` = palet
// aktif (mode × aksen, bisa berubah saat runtime).
//
// Widget TIDAK boleh meng-include file di `theme/` secara langsung untuk
// mengambil warna — pakai `p.theme.<role>` supaya pergantian tema berlaku.
// Yang boleh dipakai langsung: konstanta jarak/radius/ukuran (statis).
#ifndef KWIDGET_THEME_THEME_HPP
#define KWIDGET_THEME_THEME_HPP

#include "theme/colors.hpp"
#include "theme/typography.hpp"
#include "theme/metrics.hpp"
#include "theme/elevation.hpp"
#include "theme/motion.hpp"

#endif // KWIDGET_THEME_THEME_HPP
