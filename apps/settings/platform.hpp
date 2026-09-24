// apps/settings/platform.hpp — satu-satunya tempat app ini menyentuh header
// platform C (pola apps/filemanager/platform.hpp dan apps/gallery/platform.hpp).
//
// userlib.h/libgui.h adalah header C: WAJIB di-include dengan linkage C dari TU
// C++, kalau tidak deklarasinya menjadi C++ (nama termangled) dan link ke simbol
// syscall/libgui gagal. libui.h/kzfont*.h sudah punya guard extern "C" sendiri.
#ifndef SETTINGS_PLATFORM_HPP
#define SETTINGS_PLATFORM_HPP

extern "C" {
#include "userlib.h"   // sys_* + ABI filesystem/FD
#include "libgui.h"
}
#include "libui.h"         // toolkit widget (ABI C, sudah extern "C")
#include "color_utils.h"   // palet + COLOR_RGB_INIT (libs/gui/color)
#include "kzfont.h"        // teks FreeType (preview font)
#include "kzfonts.h"       // registry font UI (single source of truth)
#include "kzfontcfg.h"     // format font.ui (tag + id)

#endif // SETTINGS_PLATFORM_HPP
