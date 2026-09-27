// apps/taskmgr/platform.hpp — satu-satunya tempat app ini menyentuh header
// platform C (pola apps/settings/platform.hpp dan apps/filemanager/platform.hpp).
//
// userlib.h/libgui.h adalah header C: WAJIB di-include dengan linkage C dari TU
// C++, kalau tidak deklarasinya menjadi C++ (nama termangled) dan link ke simbol
// syscall/libgui gagal. libui.h sudah punya guard extern "C" sendiri.
#ifndef TASKMGR_PLATFORM_HPP
#define TASKMGR_PLATFORM_HPP

extern "C" {
#include "userlib.h"   // sys_proc_list/sys_kill/sys_getpid/sys_total_ram/...
#include "libgui.h"
}
#include "libui.h"         // toolkit widget (ABI C, sudah extern "C")

#endif // TASKMGR_PLATFORM_HPP
