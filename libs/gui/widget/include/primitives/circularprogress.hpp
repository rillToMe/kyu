// libs/gui/widget/include/primitives/circularprogress.hpp — progres melingkar.
//
// KENAPA WIDGET TOOLKIT: sama alasannya dengan AnalogClock — butuh token tema
// dan Painter (UI-2.1). Dipakai Timer, dan memang generik: setiap kali sebuah
// durasi perlu ditampilkan sebagai busur, ini primitifnya.
//
// BENTUK (UI-10.1, UI-10.4): busur tipis di atas track redup. TANPA gradien,
// TANPA glow, TANPA bayangan. Yang membuatnya terbaca adalah panjang busur,
// bukan efek.
//
// NILAI 0..1000 (per-mil, bukan persen): timer 60 menit yang diperbarui tiap
// 100 ms bergerak ~0.028% per langkah — dengan persen bulat, busur akan
// terlihat tersendat. Presisi per-mil cukup untuk semua durasi praktis dan
// tetap integer.
#ifndef KWIDGET_PRIMITIVES_CIRCULARPROGRESS_HPP
#define KWIDGET_PRIMITIVES_CIRCULARPROGRESS_HPP

#include "core/widget.hpp"
#include "core/painter.hpp"
#include "theme/trig.hpp"

namespace ui {

class CircularProgress : public Widget {
public:
    // 0..1000. Nilai dijepit; di luar rentang tidak pernah menghasilkan busur
    // yang melewati satu putaran penuh.
    int value;
    int thickness;

    CircularProgress(int size)
        : value(0), thickness(clockface::PROGRESS_W) {
        w = size; h = size;
    }

    void set_size(int size) {
        if (size < 16) size = 16;
        w = size; h = size;
        mark_dirty();
    }

    void set_value(int v) {
        if (v < 0) v = 0;
        if (v > 1000) v = 1000;
        if (v == value) return;
        value = v;
        mark_dirty();
    }

    virtual bool focusable() override { return false; }
    virtual void set_hover(bool on) override { (void)on; }

    virtual void draw(Painter& p) override {
        if (!visible || w <= 0 || h <= 0) return;
        const Metrics& m = p.theme.metrics;
        int size = (w < h) ? w : h;
        int cx = x + w / 2;
        int cy = y + h / 2;
        int t = thickness > 0 ? thickness : 1;
        // Busur ditaruh di dalam tepi supaya tidak ada piksel yang keluar
        // bounds widget (UI-19.4: damage tracking bergantung padanya).
        int r = size / 2 - m.progress_gap - t / 2;
        if (r <= t) return;

        // Muka = latar halaman; track = surface_variant (permukaan TERBENAM,
        // arahnya ke bg — UI-16.3). Bukan surface_elevated: track adalah
        // "alur", bukan permukaan yang terangkat.
        p.circle_fill(cx, cy, r, p.theme.bg);
        p.circle_ring(cx, cy, r, t, p.theme.surface_variant);

        if (value <= 0) return;
        // Busur mulai di JAM 12 dan berjalan searah jarum jam. arc_stroke
        // memakai konvensi trigonometri (0 = jam 3), jadi titik awalnya
        // digeser ke ANGLE_12.
        int32_t sweep = (int32_t)((int64_t)value * ANGLE_FULL / 1000);
        if (sweep <= 0) return;
        if (sweep >= ANGLE_FULL) {
            // Selesai: cincin penuh, jadi tidak ada celah di sambungan.
            p.circle_ring(cx, cy, r, t, p.theme.accent);
            return;
        }
        p.arc_stroke(cx, cy, r, t, ANGLE_12, sweep, p.theme.accent);
    }
};

} // namespace ui

#endif // KWIDGET_PRIMITIVES_CIRCULARPROGRESS_HPP
