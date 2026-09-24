// libdesktop — event ternormalisasi (header-only).
//
// Aplikasi desktop TIDAK memanggil pompa event kernel langsung dan tidak
// mengenal ABI syscall mentah (nomor syscall, register, struct event C).
// Backend EventPoller menerjemahkan event kernel ke representasi ini.
#ifndef KYUZEN_DESKTOP_EVENT_HPP
#define KYUZEN_DESKTOP_EVENT_HPP

#include <stdint.h>
#include <kyuzen/desktop/geometry.hpp>

namespace kyuzen {
namespace desktop {

enum class EventType {
    None,
    MouseMove,
    MouseButton,
    Key,
    Window,  // cakupan: hanya Quit (EVENT_WIN_CLOSE) hari ini
    Quit,
    WallpaperReload,  // EVENT_WALLPAPER_RELOAD (syscall 84, LEGACY): muat ulang
                      // wallpaper dari konfigurasi persisten
    HotReload,        // EVENT_HOT_RELOAD (syscall 85): reload generik;
                      // hot_target = kz_hot_reload_target (kwm_abi.h)
};

struct Event {
    EventType type;
    Point pos;          // MouseMove: posisi kursor (koordinat layar)
    int button;         // MouseButton: 0 = kiri, 1 = kanan, ...
    bool pressed;       // MouseButton: true = tekan, false = lepas
    int key;            // Key: ASCII (0 = non-printable)
    int modifiers;      // Key: bitmask KEY_MOD_* (diteruskan apa adanya)
    uint32_t window_id;  // Window/Quit: id window terkait (0 = tak relevan)
    uint32_t hot_target;  // HotReload: target reload (0 = tak relevan)
};

inline Event no_event() {
    Event e;
    e.type = EventType::None;
    e.pos.x = 0;
    e.pos.y = 0;
    e.button = 0;
    e.pressed = false;
    e.key = 0;
    e.modifiers = 0;
    e.window_id = 0;
    e.hot_target = 0;
    return e;
}

// Pompa event kernel. poll() mengisi `out` dan mengembalikan true bila ada
// event; false berarti antrean kosong (bukan error) — panggil tiap iterasi,
// jangan block. Satu-satunya titik yang menyentuh ABI event mentah.
class EventPoller {
public:
    bool poll(Event& out);
};

}  // namespace desktop
}  // namespace kyuzen

#endif
