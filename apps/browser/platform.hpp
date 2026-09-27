// apps/browser/platform.hpp — satu-satunya tempat app ini menyentuh header
// platform C (pola taskmgr/settings/filemanager).
//
// userlib.h/libgui.h/kzfont.h/netutil.h adalah header C: WAJIB linkage C dari
// TU C++. libui.h/libui_xml.h sudah punya guard extern "C" sendiri.
// <string> WAJIB sebelum header platform apa pun.
#ifndef BROWSER_PLATFORM_HPP
#define BROWSER_PLATFORM_HPP

#include <cstdint>
#include <string>

extern "C" {
#include "userlib.h"    // sys_*: socket/connect/send/recv/resolve/exit, print
#include "libgui.h"     // gui_damage_rect (dipakai viewport? tidak — FtText)
#include "netutil.h"    // net_resolve (transport)
#include "net_socket.h"  // KSOCK_* (pemetaan error transport)
#include "media.h"       // image_decode_memory (gambar halaman)
#include "kzfont.h"     // teks FreeType viewport
#include "kzfonts.h"    // registry font FS (/Inter-Regular.ttf)
#include "color_types.h"  // color_t, color_from_u32
}
#include "libui.h"      // toolkit widget (ABI C)
#include "libui_xml.h"  // chrome deklaratif

#endif  // BROWSER_PLATFORM_HPP
