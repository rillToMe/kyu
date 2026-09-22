// libdesktop backend — Application: satu-satunya pemilik event loop.
//
// Urutan run(): gui_create_desktop → Canvas::Impl menunjuk window →
// on_start → render Full + flush → loop:
//   drain event (semua yang antre) → on_event tiap event, akumulasi damage
//   tiap 10 iterasi → on_poll, akumulasi damage
//   damage != None → shell.render(canvas, damage) + flush
//   yield
// Berhenti bila !shell.is_running() → kembalikan 0. Gagal buat window → 1
// (kasus desktop kedua yang konflik; OOM alokasi Impl).
//
// Impl dialokasikan via operator new (arena SDK malloc); window libgui via
// sys_alloc pool-nya sendiri — dua arena tak bercampur, destruksi eksplisit.
#include <kyuzen/desktop/application.hpp>
#include <kyuzen/desktop/canvas.hpp>
#include <kyuzen/desktop/event.hpp>
#include <kyuzen/desktop/shell.hpp>
#include <kyuzen/desktop/system.hpp>

extern "C" {
#include "libgui.h"
}

namespace kyuzen {
namespace desktop {

namespace {

Damage worse(Damage a, Damage b) {
    if (a == Damage::Full || b == Damage::Full) return Damage::Full;
    if (a == Damage::Partial || b == Damage::Partial) return Damage::Partial;
    return Damage::None;
}

}  // namespace

int Application::run(Shell& shell) {
    gui_window_t* win = gui_create_desktop();
    if (!win) return 1;

    Canvas canvas;
    canvas.attach(win);

    EventPoller poller;
    Event ev = no_event();

    shell.on_start(canvas);
    shell.render(canvas, Damage::Full);
    canvas.flush();

    unsigned frame = 0;
    while (shell.is_running()) {
        Damage d = Damage::None;
        while (poller.poll(ev)) {
            Damage ed = shell.on_event(ev, canvas);
            d = worse(d, ed);
        }
        frame++;
        if (frame % 10 == 0) {
            Damage pd = shell.on_poll(canvas);
            d = worse(d, pd);
        }
        if (d != Damage::None) {
            shell.render(canvas, d);
            canvas.flush();
        }
        System::yield();
    }
    return 0;
}

}  // namespace desktop
}  // namespace kyuzen
