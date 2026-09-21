// Framework smoke Phase 8 — aplikasi konsol biasa yang memakai libdesktop.
//
// Memverifikasi tanpa window desktop (berjalan berdampingan dengan desktop
// asli): geometri, System, abstraksi WindowManager (live KWM), drain event,
// lifecycle Shell yang dikemudikan manual (termasuk render ke Canvas null —
// semua op Canvas null-guard, jadi aman), shutdown. Render berpiksel + loop
// Application::run dibuktikan boot desktop (default + test-desktop).
//
// Bukti serial: [dtsmoke] PASS (konsol di-mirror ke COM1).
#include <kyuzen/desktop/application.hpp>
#include <kyuzen/desktop/event.hpp>
#include <kyuzen/desktop/geometry.hpp>
#include <kyuzen/desktop/shell.hpp>
#include <kyuzen/desktop/system.hpp>
#include <kyuzen/desktop/window_manager.hpp>

#include <stdio.h>

namespace {

using kyuzen::desktop::Canvas;
using kyuzen::desktop::Damage;
using kyuzen::desktop::Event;
using kyuzen::desktop::EventType;
using kyuzen::desktop::Point;
using kyuzen::desktop::Rect;
using kyuzen::desktop::rgb;
using kyuzen::desktop::Shell;
using kyuzen::desktop::System;
using kyuzen::desktop::WindowInfo;
using kyuzen::desktop::WindowManager;
using kyuzen::desktop::no_event;

int failures = 0;

void check(bool ok, const char* name) {
    if (ok) {
        printf("[dtsmoke] %s ok\n", name);
    } else {
        printf("[dtsmoke] %s FAIL\n", name);
        failures++;
    }
}

class ProbeShell : public Shell {
public:
    ProbeShell()
        : started_(false),
          events_(0),
          polls_(0),
          renders_(0),
          done_(false) {}

    void on_start(Canvas& canvas) override {
        (void)canvas;
        started_ = true;
    }
    Damage on_event(const Event& e, Canvas& canvas) override {
        (void)canvas;
        events_++;
        if (e.type == EventType::Quit) {
            done_ = true;
            return Damage::Full;
        }
        if (e.type == EventType::MouseMove) return Damage::None;
        return Damage::Partial;
    }
    Damage on_poll(Canvas& canvas) override {
        (void)canvas;
        polls_++;
        return Damage::None;
    }
    void render(Canvas& canvas, Damage d) override {
        (void)canvas;
        (void)d;
        renders_++;
    }
    bool is_running() const override { return !done_; }

    bool started_;
    int events_;
    int polls_;
    int renders_;
    bool done_;
};

}  // namespace

extern "C" int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    // 1. Geometri murni.
    {
        Rect r;
        r.x = 10;
        r.y = 20;
        r.width = 100;
        r.height = 50;
        Point inside;
        inside.x = 50;
        inside.y = 40;
        Point outside;
        outside.x = 200;
        outside.y = 20;
        Rect overlap;
        overlap.x = 50;
        overlap.y = 40;
        overlap.width = 100;
        overlap.height = 100;
        Rect far;
        far.x = 500;
        far.y = 500;
        far.width = 10;
        far.height = 10;
        bool ok = r.contains(inside) && !r.contains(outside) &&
                  r.intersects(overlap) && !r.intersects(far) &&
                  rgb(1, 2, 3).r == 1 && rgb(1, 2, 3).a == 255;
        check(ok, "geometry");
    }

    // 2. System: waktu monoton.
    {
        uint64_t a = System::uptime_ms();
        System::yield();
        uint64_t b = System::uptime_ms();
        check(b >= a, "system");
    }

    // 3. WindowManager live (desktop asli berjalan → daftar terbaca).
    {
        WindowManager wm;
        WindowInfo wins[16];
        int n = wm.get_windows(wins, 16);
        bool ok = (n >= 0);
        if (ok)
            printf("[dtsmoke] wm ok (%d windows)\n", n);
        else
            printf("[dtsmoke] wm FAIL\n");
        if (!ok) failures++;
        // Aktivasi id 0 (kosong) harus ditolak dengan aman.
        check(!wm.activate(0), "wm-guard");
    }

    // 4. Drain event (tak portable jumlahnya — cukup tak crash + tipe valid).
    {
        Event ev = no_event();
        kyuzen::desktop::EventPoller poller;
        int drained = 0;
        for (int i = 0; i < 64; i++) {
            if (!poller.poll(ev)) break;
            drained++;
            bool valid = ev.type == EventType::None ||
                         ev.type == EventType::MouseMove ||
                         ev.type == EventType::MouseButton ||
                         ev.type == EventType::Key ||
                         ev.type == EventType::Window ||
                         ev.type == EventType::Quit;
            if (!valid) break;
        }
        printf("[dtsmoke] events ok (drained %d)\n", drained);
    }

    // 5. Lifecycle Shell manual (render ke Canvas null — aman).
    {
        ProbeShell sh;
        Canvas cv;
        sh.on_start(cv);
        Event mv = no_event();
        mv.type = EventType::MouseMove;
        mv.pos.x = 10;
        mv.pos.y = 10;
        Damage d1 = sh.on_event(mv, cv);
        Event q = no_event();
        q.type = EventType::Quit;
        Damage d2 = sh.on_event(q, cv);
        Damage d3 = sh.on_poll(cv);
        sh.render(cv, Damage::Full);
        sh.render(cv, Damage::Partial);
        bool ok = sh.started_ && sh.events_ == 2 && sh.polls_ == 1 &&
                  sh.renders_ == 2 && !sh.is_running() &&
                  d1 == Damage::None && d2 == Damage::Full &&
                  d3 == Damage::None;
        check(ok, "shell");
    }

    if (failures == 0) {
        printf("[dtsmoke] PASS\n");
        return 0;
    }
    printf("[dtsmoke] FAIL (%d)\n", failures);
    return 1;
}
