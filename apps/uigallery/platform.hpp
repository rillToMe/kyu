// apps/uigallery/platform.hpp — satu-satunya tempat app ini menyentuh header
// platform C (pola apps/settings/platform.hpp).
//
// userlib.h/libgui.h adalah header C: WAJIB di-include dengan linkage C dari TU
// C++, kalau tidak deklarasinya menjadi C++ (nama termangled) dan link ke simbol
// syscall/libgui gagal. libui.h sudah punya guard extern "C" sendiri.
#ifndef UIGALLERY_PLATFORM_HPP
#define UIGALLERY_PLATFORM_HPP

extern "C" {
#include "userlib.h"   // sys_* + ABI filesystem/FD
#include "libgui.h"
}
#include "libui.h"         // toolkit widget (ABI C, sudah extern "C")
#include "color_utils.h"   // COLOR_HEX / color_hex (libs/gui/color)

#endif // UIGALLERY_PLATFORM_HPP
