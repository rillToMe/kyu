// libs/gui/widget/include/primitives/analogclock.hpp — muka jam analog.
//
// KENAPA WIDGET TOOLKIT, BUKAN GAMBAR DI DALAM APLIKASI (UI-2.1, UI-13.11):
// muka jam harus membaca token tema (warna jarum, warna cincin, warna teks)
// dan menggambar lewat Painter. Aplikasi tidak punya akses yang sah ke
// keduanya; menyalinnya ke app akan mengunci warna di dalam kode aplikasi dan
// melanggar aturan "nilai visual hanya dari token".
//
// BENTUK: instrumen minimal, bukan jam alarm skeuomorfik (UI-10.1). Yang
// digambar hanya: cincin tipis, goresan jam + menit, tiga jarum, dan pivot.
// Tanpa tekstur, tanpa gradien, tanpa bayangan, tanpa angka.
//
// WAKTU DIMASUKKAN DARI LUAR (set_time), bukan dibaca widget sendiri: widget
// tidak boleh memanggil syscall (UI-13.3), dan pemisahan ini yang membuat
// posisi jarum bisa diuji di host tanpa jam sistem.
#ifndef KWIDGET_PRIMITIVES_ANALOGCLOCK_HPP
#define KWIDGET_PRIMITIVES_ANALOGCLOCK_HPP

#include "core/widget.hpp"
#include "core/painter.hpp"
#include "theme/trig.hpp"

namespace ui {

class AnalogClock : public Widget {
public:
    // Sudut jarum dalam 1/65536 putaran (0 = jam 12, searah jarum jam).
    int32_t ang_hour, ang_min, ang_sec;
    bool show_seconds;
    // Langkah kuantisasi sudut detik: sudut hanya berubah saat melewati
    // kelipatan ini, sehingga tick loop tidak memicu render tiap iterasi.
    int sweep_step;
    // Sudut terakhir yang benar-benar DIGAMBAR (dipakai set_time untuk
    // memutuskan perlu render atau tidak).
    int32_t last_drawn_sec;

    AnalogClock(int size)
        : ang_hour(0), ang_min(0), ang_sec(0), show_seconds(true),
          sweep_step(clockface::SWEEP_STEP), last_drawn_sec(-1) {
        w = size; h = size;
    }

    void set_size(int size) {
        if (size < 24) size = 24;
        w = size; h = size;
        mark_dirty();
    }

    // Setel posisi jarum dari waktu LOKAL. `ms_frac` = pecahan detik (0..999)
    // supaya jarum detik bergerak halus alih-alih melompat tiap detik
    // (persyaratan "smooth second hand").
    //
    // Return true bila tampilan benar-benar berubah. Pemanggil memakainya
    // untuk me-render hanya saat perlu (UI-20.1) — tanpa ini, loop 60 Hz
    // menggambar ulang muka jam 60x/detik walau jarumnya belum bergerak.
    bool set_time(int hour24, int minute, int second, int ms_frac) {
        if (hour24 < 0) hour24 = 0;
        hour24 %= 24;
        if (minute < 0) minute = 0;
        if (second < 0) second = 0;
        if (ms_frac < 0) ms_frac = 0;
        if (ms_frac > 999) ms_frac = 999;

        // Sudut diukur dari jam 12 (clock_angle menangani pergeseran dari
        // konvensi trigonometri yang menaruh 0 di jam 3).
        // Jam: posisi termasuk pecahan menit (jam 3:30 -> di antara 3 dan 4).
        int64_t min_total = (int64_t)minute * 65536 + (int64_t)second * 65536 / 60;
        ang_min = clock_angle(min_total, 60 * 65536);

        int64_t h_total = (int64_t)(hour24 % 12) * 65536 + min_total / 60;
        ang_hour = clock_angle(h_total, 12 * 65536);

        // Detik: pakai pecahan milidetik. Kuantisasi ke sweep_step supaya
        // tidak ada render untuk perbedaan yang tak terlihat.
        int64_t sec_total = (int64_t)second * 65536 +
                            (int64_t)ms_frac * 65536 / 1000;
        int32_t a = clock_angle(sec_total, 60 * 65536);
        if (sweep_step > 0) a = (a / sweep_step) * sweep_step;
        ang_sec = a;

        if (ang_sec == last_drawn_sec) return false;
        // Jangkar `last_drawn_sec` dipasang di DRAW, bukan di sini: kalau
        // widget tersembunyi, kita tidak boleh mengklaim sudah menggambar.
        return true;
    }

    // Dipanggil pemanggil setelah render benar-benar terjadi.
    void commit_drawn() { last_drawn_sec = ang_sec; }

    virtual bool focusable() override { return false; }
    // Muka jam tidak menerima klik: tidak ada interaksi di atasnya. Menjadikan
    // fokusable akan menelan Tab dan panah (UI-9.9).
    virtual void set_hover(bool on) override { (void)on; }

    virtual void draw(Painter& p) override {
        if (!visible || w <= 0 || h <= 0) return;
        last_drawn_sec = ang_sec;   // sudah digambar (lihat set_time)

        const Metrics& m = p.theme.metrics;
        // Muka selalu kotak: ambil sisi terkecil supaya lingkaran tidak
        // menjadi elips bila widget direntangkan layout.
        int size = (w < h) ? w : h;
        int cx = x + w / 2;
        int cy = y + h / 2;
        int r = size / 2 - 1;
        if (r < 8) return;

        // Muka = latar halaman (jam duduk di alur halaman, bukan kartu
        // mengambang) + cincin tipis sebagai batas.
        p.circle_fill(cx, cy, r, p.theme.bg);
        p.circle_ring(cx, cy, r, m.face_ring_w, p.theme.border);

        // Goresan: menit tipis-pendek, jam tebal-panjang. Digambar dari tepi
        // ke DALAM supaya tidak ada goresan yang bocor ke luar lingkaran.
        int r_in = r - m.face_tick_inset;
        for (int i = 0; i < m.face_tick_minor; i++) {
            bool major = (i % (m.face_tick_minor / m.face_tick_major)) == 0;
            int len = major ? r_in * m.face_tick_major_pct / 100
                            : r_in * m.face_tick_minor_pct / 100;
            if (len <= 0) continue;
            int tw = major ? m.face_tick_major_w : m.face_tick_minor_w;
            int32_t a = clock_angle(i, m.face_tick_minor);
            int ox, oy, ix, iy;
            point_on_circle(cx, cy, r_in, a, &ox, &oy);
            point_on_circle(cx, cy, r_in - len, a, &ix, &iy);
            // Goresan jam memakai teks primer, menit teks tersier: hierarki
            // lewat tone, bukan lewat warna baru (UI-6.4).
            p.line_thick(ox, oy, ix, iy, tw,
                         major ? p.theme.text_secondary : p.theme.text_tertiary);
        }

        // Jarum: digambar dari ekor (di seberang pusat) ke ujung.
        int tail = r * m.hand_tail_pct / 100;
        drawHand(p, cx, cy, r * m.hand_hour_pct / 100, tail, ang_hour,
                 m.hand_hour_w, p.theme.text);
        drawHand(p, cx, cy, r * m.hand_min_pct / 100, tail, ang_min,
                 m.hand_min_w, p.theme.text);
        if (show_seconds)
            drawHand(p, cx, cy, r * m.hand_sec_pct / 100, tail, ang_sec,
                     m.hand_sec_w, p.theme.accent);

        // Pivot: cakram warna teks + inti aksen. Menutup ujung jarum di pusat
        // sehingga tidak ada persilangan yang terlihat kasar.
        p.circle_fill(cx, cy, m.face_pivot_r, p.theme.text);
        if (m.face_pivot_inner_r > 0)
            p.circle_fill(cx, cy, m.face_pivot_inner_r, p.theme.bg);
    }

private:
    // Satu jarum: dari (pusat - tail) ke (pusat + len), tebal t.
    void drawHand(Painter& p, int cx, int cy, int len, int tail, int32_t angle,
                  int t, color_t c) {
        if (len <= 0) return;
        int tx, ty, ex, ey;
        point_on_circle(cx, cy, tail, angle + ANGLE_HALF, &tx, &ty);
        point_on_circle(cx, cy, len, angle, &ex, &ey);
        p.line_thick(tx, ty, ex, ey, t, c);
    }
};

} // namespace ui

#endif // KWIDGET_PRIMITIVES_ANALOGCLOCK_HPP
