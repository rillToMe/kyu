// user_apps/gallery/platform.hpp — satu-satunya tempat app ini menyentuh
// header platform C.
//
// userlib.h/libgui.h adalah header C: WAJIB di-include dengan linkage C dari
// TU C++, kalau tidak deklarasinya menjadi C++ (nama termangled) dan link ke
// simbol syscall/libgui gagal. Pola yang sama dipakai toolkit
// (libs/widget/include/runtime/platform.hpp) dan libdesktop (canvas.cpp).
//
// libui.h dan media.h sudah punya guard extern "C" sendiri.
#ifndef GALLERY_PLATFORM_HPP
#define GALLERY_PLATFORM_HPP

extern "C" {
#include "userlib.h"
#include "libgui.h"
}
#include "libui.h"
#include "media.h"
#include "media_scale.h"   // scaler RGBA bersama (static inline, sama dgn desktop)

#endif // GALLERY_PLATFORM_HPP
