#ifndef KWM_ABI_H
#define KWM_ABI_H

#include <stdint.h>
#include <stddef.h>   // offsetof — freeze asserts di bawah

// ============================================================
// KWM ABI — tipe bersama kernel <-> user (pola crash_notice.h).
//
// kwm_window_info_t dipakai di DUA sisi: kernel (kwm.h: kwm_get_windows,
// sys_kwm.c: bounce copy-out syscall 61) dan user (userlib.h: wrapper
// sys_kwm_get_windows, libdesktop, app). SATU definisi kanonis di sini;
// kwm.h dan userlib.h hanya include file ini. Jangan duplikasi lagi.
//
// Layout FROZEN (syscall 61 ABI): win_id = slot KWM + 1, 0 = kosong.
// Assert di bawah mengunci sizeof + offset; drift apa pun = ABI rusak.
// Baseline terukur: sizeof=68, off 0/4/5/8/12/16/20/24/28/32/36.
// ============================================================

typedef struct {
    uint32_t win_id;      // slot KWM + 1; 0 = kosong (konvensi event win_id)
    uint8_t  active;
    uint8_t  focused;     // 1 = pemegang fokus keyboard (tint titlebar)
    int32_t  x, y;
    uint32_t width, height, z_index;
    int32_t  owner_task;
    uint32_t flags;
    char     title[32];
} kwm_window_info_t;      // ABI x86_64, tanpa #pragma pack

_Static_assert(sizeof(kwm_window_info_t) == 68, "kwm_window_info_t size drift");
_Static_assert(offsetof(kwm_window_info_t, win_id) == 0, "abi off win_id");
_Static_assert(offsetof(kwm_window_info_t, active) == 4, "abi off active");
_Static_assert(offsetof(kwm_window_info_t, focused) == 5, "abi off focused");
_Static_assert(offsetof(kwm_window_info_t, x) == 8, "abi off x");
_Static_assert(offsetof(kwm_window_info_t, y) == 12, "abi off y");
_Static_assert(offsetof(kwm_window_info_t, width) == 16, "abi off width");
_Static_assert(offsetof(kwm_window_info_t, height) == 20, "abi off height");
_Static_assert(offsetof(kwm_window_info_t, z_index) == 24, "abi off z_index");
_Static_assert(offsetof(kwm_window_info_t, owner_task) == 28, "abi off owner");
_Static_assert(offsetof(kwm_window_info_t, flags) == 32, "abi off flags");
_Static_assert(offsetof(kwm_window_info_t, title) == 36, "abi off title");

#endif // KWM_ABI_H
