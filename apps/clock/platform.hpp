// apps/clock/platform.hpp — satu-satunya tempat aplikasi ini menyentuh header
// platform C (pola yang sama dengan browser/taskmgr/settings/filemanager).
//
// userlib.h, kzfont.h, kzfonts.h, kzfontcfg.h adalah header C TANPA guard
// extern "C" — kalau di-include langsung dari TU C++, nama fungsinya ter-mangle
// dan link gagal dengan "undefined symbol: sys_free(void*)" (persis yang
// terjadi sebelum header ini ada). libui.h/libui_xml.h/kztime.h sudah punya
// guard sendiri, jadi keduanya boleh di-include di luar blok ini.
#ifndef CLOCK_PLATFORM_HPP
#define CLOCK_PLATFORM_HPP

extern "C" {
#include "userlib.h"      // sys_*: waktu, uptime, file, spawn, exit
#include "kzfont.h"       // kz_font_*: font hero milik aplikasi
#include "kzfonts.h"      // registry font (/Inter-Regular.ttf)
#include "kzfontcfg.h"    // format /font.ui
#include "color_types.h"  // color_t
}

#include "libui.h"        // toolkit widget (ABI C, sudah extern "C")
#include "libui_xml.h"    // UI deklaratif
#include "kztime.h"       // kalender + zona waktu (sudah extern "C")

#endif // CLOCK_PLATFORM_HPP
