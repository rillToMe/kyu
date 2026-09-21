// libdesktop — abstraksi KWM minimal (tipe milik framework; detail KWM dan
// syscall window-manager mentah TIDAK bocor ke aplikasi).
#ifndef KYUZEN_DESKTOP_WINDOW_MANAGER_HPP
#define KYUZEN_DESKTOP_WINDOW_MANAGER_HPP

#include <stdint.h>

namespace kyuzen {
namespace desktop {

// Satu baris daftar window untuk taskbar. Ukuran/title dibatasi seperti ABI
// KWM (judul 32 char) — batas ini milik kontrak WM, bukan kebijakan desktop.
struct WindowInfo {
    uint32_t id;  // slot KWM + 1 (konvensi id event KWM; 0 = kosong)
    char title[32];
    bool focused;
    bool is_desktop;  // window shell sendiri — taskbar melewatinya
};

class WindowManager {
public:
    // Daftar window saat ini (maks `max`). Mengembalikan jumlah, atau -1
    // bila query gagal (out tak diubah).
    int get_windows(WindowInfo* out, int max);
    // Bawa window ke depan + fokus (id = WindowInfo::id apa adanya;
    // penerjemahan slot+1 → slot mentah milik backend). false = ditolak.
    bool activate(uint32_t id);
};

}  // namespace desktop
}  // namespace kyuzen

#endif
