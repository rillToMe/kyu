// libdesktop — batas shell yang bisa diganti (framework milik lifecycle,
// implementasi milik kebijakan).
//
// Framework memiliki: startup, polling event, dispatch, shutdown.
// Implementasi memiliki: apa yang digambar, respons input, launcher,
// taskbar, wallpaper, state khas desktop.
//
// Render dua jalur (Full vs Partial) BUKAN konsep taskbar di framework:
// Full = "gambar ulang semuanya", Partial = "gambar ulang area dinamis
// saja". Implementasi default memetakan Partial ke strip taskbar; desktop
// lain bebas memetakan Partial ke Full.
#ifndef KYUZEN_DESKTOP_SHELL_HPP
#define KYUZEN_DESKTOP_SHELL_HPP

#include <kyuzen/desktop/canvas.hpp>
#include <kyuzen/desktop/event.hpp>

namespace kyuzen {
namespace desktop {

class Shell {
public:
    virtual ~Shell() = default;

    // Sekali saat startup (setelah surface siap). Tempat probe awal
    // (scan aplikasi, notifikasi crash) + render Full pertama.
    virtual void on_start(Canvas& canvas) {}

    // Satu event → gambar seperlunya → kembalikan damage yang terjadi.
    virtual Damage on_event(const Event& e, Canvas& canvas) = 0;

    // Polling berkala (framework memanggil tiap ~10 iterasi): query daftar
    // window, timer, re-scan. Default: tak ada kerja → tak ada damage.
    virtual Damage on_poll(Canvas& canvas) { return Damage::None; }

    // Gambar ulang. Dipanggil framework hanya bila damage != None.
    virtual void render(Canvas& canvas, Damage d) = 0;

    // false = keluar loop (framework lalu shutdown). Desktop normal
    // berjalan selamanya; test-desktop memakainya untuk berhenti.
    virtual bool is_running() const = 0;
};

}  // namespace desktop
}  // namespace kyuzen

#endif
