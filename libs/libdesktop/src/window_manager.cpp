// libdesktop backend — WindowManager di atas sys_kwm_*.
//
// Satu-satunya TU yang menyentuh sys_kwm_get_windows/activate. Mirror
// KWM_WIN_DESKTOP (0x1, lih. kernel kwm_internal.h) dimiliki backend —
// aplikasi hanya melihat WindowInfo::is_desktop.
#include <kyuzen/desktop/window_manager.hpp>

extern "C" {
#include "userlib.h"
}

namespace kyuzen {
namespace desktop {

int WindowManager::get_windows(WindowInfo* out, int max) {
    if (!out || max <= 0) return -1;
    // Buffer mentah di stack (maks 16 — batas taskbar lama; pemanggil yang
    // meminta lebih dari itu tetap dilayani sebatas buffer ini).
    const int kCap = 16;
    kwm_window_info_t raw[kCap];
    int want = max < kCap ? max : kCap;
    int n = sys_kwm_get_windows(raw, want);
    if (n < 0) return -1;
    if (n > want) n = want;
    for (int i = 0; i < n; i++) {
        out[i].id = raw[i].win_id;
        for (int j = 0; j < 31; j++) {
            out[i].title[j] = raw[i].title[j];
            if (!raw[i].title[j]) break;
        }
        out[i].title[31] = '\0';
        out[i].focused = (raw[i].focused != 0);
        out[i].is_desktop = ((raw[i].flags & 0x1u) != 0);
    }
    return n;
}

bool WindowManager::activate(uint32_t id) {
    // KWM melapor slot+1 (konvensi id event) tetapi aktivasi memakai slot
    // mentah (kwm_activate_window mengindeks langsung). Terjemahan milik
    // backend — implementasi cukup meneruskan WindowInfo::id.
    if (id == 0) return false;
    return sys_kwm_activate_window(static_cast<int>(id) - 1) == 0;
}

}  // namespace desktop
}  // namespace kyuzen
