// apps/desktop — satu-satunya titik yang menyentuh header C userspace
// (userlib.h/libgui.h) di sisi implementasi. Semua TU implementasi memakai
// header ini; backend framework punya wrap-nya sendiri di libs/libdesktop.
// Header C Kyuzen tanpa guard extern "C", jadi dibungkus di sini sekali.
#ifndef KYUZEN_DESKTOP_IMPL_SYS_ABI_HPP
#define KYUZEN_DESKTOP_IMPL_SYS_ABI_HPP

extern "C" {
#include "userlib.h"
}

#endif
