// Test Desktop — implementasi desktop pengganti yang minimal (bukti
// replaceability Phase 8).
//
// Memakai libdesktop + C++ SDK + runtime/linker yang SAMA dengan desktop
// normal, tanpa menyentuh framework. Latar solid + satu baris teks;
// boot → render → shutdown tanpa interaksi.
#include <kyuzen/desktop/application.hpp>
#include <kyuzen/desktop/shell.hpp>
#include <kyuzen/desktop/system.hpp>

// Jejak serial (konsol di-mirror ke COM1) = bukti QEMU headless.
extern "C" {
void print(char* text);
}

namespace {

using kyuzen::desktop::Application;
using kyuzen::desktop::Canvas;
using kyuzen::desktop::Damage;
using kyuzen::desktop::Event;
using kyuzen::desktop::EventType;
using kyuzen::desktop::Point;
using kyuzen::desktop::Rect;
using kyuzen::desktop::rgb;
using kyuzen::desktop::Shell;
using kyuzen::desktop::System;

const unsigned long kQuitAfterMs = 10000;

class TestShell : public Shell {
public:
    TestShell() : done_(false), t0_(0) {}

    void on_start(Canvas& canvas) override {
        (void)canvas;
        t0_ = System::uptime_ms();
        // Jejak serial (konsol di-mirror ke COM1) = bukti QEMU headless.
        // print() milik C SDK; dideklarasikan di bawah via extern "C".
        trace("[test-desktop] started (libdesktop)\n");
    }

    Damage on_event(const Event& e, Canvas& canvas) override {
        (void)canvas;
        // Tombol/key apa pun = minta keluar lebih awal (uell, tetap tenang).
        if (e.type == EventType::MouseButton && e.pressed) done_ = true;
        if (e.type == EventType::Key) done_ = true;
        if (e.type == EventType::Quit) done_ = true;
        return Damage::None;
    }

    Damage on_poll(Canvas& canvas) override {
        (void)canvas;
        if (!done_ && System::uptime_ms() - t0_ >= kQuitAfterMs) {
            done_ = true;
            trace("[test-desktop] exiting (timeout)\n");
            return Damage::Full;  // render perpisahan sebelum keluar
        }
        return Damage::None;
    }

    void render(Canvas& canvas, Damage d) override {
        (void)d;
        Rect bg;
        bg.x = 0;
        bg.y = 0;
        bg.width = canvas.width();
        bg.height = canvas.height();
        canvas.fill_rect(bg, rgb(0x1A, 0x2E, 0x1A));
        const char* msg = "Test Desktop";
        Point p;
        p.x = canvas.width() / 2 - 6 * 8;
        p.y = canvas.height() / 2 - 8;
        canvas.draw_text(msg, p, rgb(0xC0, 0xE0, 0xC0));
    }

    bool is_running() const override { return !done_; }

private:
    static void trace(const char* s) { print(const_cast<char*>(s)); }

    bool done_;
    uint64_t t0_;
};

}  // namespace

extern "C" int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    Application app;
    TestShell shell;
    int rc = app.run(shell);
    print(const_cast<char*>("[test-desktop] shutdown ok\n"));
    return rc;
}
