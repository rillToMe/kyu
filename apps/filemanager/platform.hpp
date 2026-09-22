// apps/filemanager/platform.hpp — satu-satunya tempat app ini menyentuh header
// platform C.
//
// userlib.h/libgui.h adalah header C: WAJIB di-include dengan linkage C dari TU
// C++, kalau tidak deklarasinya menjadi C++ (nama termangled) dan link ke simbol
// syscall/libgui gagal. Pola yang sama dipakai Gallery
// (apps/gallery/platform.hpp), toolkit (libs/gui/widget/include/runtime) dan
// libdesktop (canvas.cpp).
//
// libui.h dan media.h sudah punya guard extern "C" sendiri; urutan include di
// bawah penting: C dulu, baru yang sudah ber-guard.
#ifndef FM_PLATFORM_HPP
#define FM_PLATFORM_HPP

extern "C" {
#include "userlib.h"   // sys_* + ABI filesystem/FD (syscall 81-83)
#include "libgui.h"
}
#include "libui.h"         // toolkit widget (ABI C, sudah extern "C")
#include "media.h"         // tipe berkas + util path + format ukuran (libs/media/media.c)
#include "media_scale.h"   // scaler RGBA bersama (static inline)

#endif // FM_PLATFORM_HPP
