// Kyuzen Desktop — launcher: discovery /apps + manifest .app + grid ikon.
//
// Kebijakan implementasi (bukan framework): scan FS tiap *.elf, enrich via
// "<base>.app" (name/color/hidden), tanpa daftar hardcode. State eksplisit
// di objek (bukan global).
#ifndef KYUZEN_DESKTOP_IMPL_LAUNCHER_HPP
#define KYUZEN_DESKTOP_IMPL_LAUNCHER_HPP

#include <kyuzen/desktop/canvas.hpp>
#include <kyuzen/desktop/geometry.hpp>
#include <kyuzen/desktop/system.hpp>

namespace desktop_impl {

using kyuzen::desktop::Canvas;
using kyuzen::desktop::Color;
using kyuzen::desktop::Point;
using kyuzen::desktop::Rect;

const int MAX_APPS = 32;

// Satu entri launcher: label tampil + path spawn + warna ikon.
struct AppEntry {
    char label[32];
    char elf[32];  // path lengkap "/apps/<nama>"
    Color color;
};

// "0x1565C0" (atau desimal) → Color opaque. Berhenti di karakter non-digit.
// Diekspos untuk host test (perilaku parse = kontrak).
Color parse_color(const char* s);

class Launcher {
public:
    Launcher();

    // Scan /apps; true = hasil beda dari sebelumnya (perlu render Full).
    bool discover();
    int count() const { return napps_; }
    const AppEntry& entry(int i) const { return apps_[i]; }

    // Geometri grid (murni, tanpa state layar): kolom yang muat di lebar w,
    // kapasitas ikon di atas taskbar untuk layar w×h.
    static int grid_cols(int w);
    int grid_cap(int w, int h) const;
    static Rect icon_rect(int i, int cols);
    // Indeks ikon di titik p, atau -1 (di luar ikon / di luar kapasitas).
    int find_icon(Point p, int cols, int cap) const;

    void draw(Canvas& canvas) const;

private:
    AppEntry apps_[MAX_APPS];
    int napps_;
    unsigned checksum_;
};

}  // namespace desktop_impl

#endif
